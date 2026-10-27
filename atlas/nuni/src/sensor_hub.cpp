#include "nuni/sensor_hub.hpp"
#include "nuni/topics.hpp"
#include <cmath>
namespace nuni {
SensorHub::SensorHub(Publish publish,std::string boot,double start,TofWatchConfig tof)
  : publish_(std::move(publish)),boot_(std::move(boot)),start_(start),tof_(tof) {}
bool SensorHub::on_line(const std::string& channel,const std::string& line,double now) {
  if (line.size()>4096 || (channel!="env" && channel!="radar")) return false;
  const auto marker=line.find("NUNI1 ");
  if (marker==std::string::npos) return false;
  const auto end=line.rfind('}'); // ESPHome may append ANSI logger formatting
  if(end==std::string::npos || end<marker+6)return false;
  auto parsed=Json::parse(line.substr(marker+6,end-marker-5),nullptr,false);
  if (!parsed.is_object()) return false;
  try {
    if (parsed.value("v",0)!=1 || parsed.value("device","")!=channel ||
        !parsed.contains("seq") || !parsed["seq"].is_number_integer() ||
        !parsed.contains("data") || !parsed["data"].is_object()) return false;
    const auto boot=parsed.value("boot_id",""); const auto seq=parsed["seq"].get<long long>();
    const auto kind=parsed.value("kind","");
    const auto uptime=as_number(pyget(parsed,"uptime_ms"));
    if(!uptime || !std::isfinite(*uptime) || *uptime<0)return false;
    if (boot.empty() || boot.size()>96 || seq<0) return false;
    if (channel=="env" && kind!="env" && kind!="tof" && kind!="relay") return false;
    if (channel=="radar" && kind!="presence" && kind!="breath_rate" && kind!="heart_rate" && kind!="distance") return false;
    auto& source=sources_[channel];
    if (source.boot!=boot) {
      if (source.retired.count(boot)) return false;
      if (!source.boot.empty()) source.retired.insert(source.boot);
      if(source.retired.size()>64) return false;
      source.boot=boot; source.seq=-1; source.offset=now-*uptime/1000;
      if(channel=="radar") radar_=RadarBridge();
      else { tof_.reset(); env_=nullptr; env_at_=-1; tof_at_=-1; relay_=nullptr;relay_at_=-1;relay_sequence_=0; }
    }
    if(seq<=source.seq) return false;
    source.seq=seq;
    source.offset=std::min(source.offset,now-*uptime/1000);
    const auto measured_at=source.offset+*uptime/1000;
    if(now-measured_at>2.5)return false; // serial backlog must not become fresh observations
    Json data=parsed["data"];
    data["sample_id"]=boot+":"+std::to_string(seq);
    if(channel=="radar") {
      const Json value=pyget(data,"value");
      if(kind=="presence") {
        if(!value.is_boolean()) return false;
        radar_.on_esphome("mr60bha2/presence",value.get<bool>()?"ON":"OFF",measured_at);
      } else {
        if(!value.is_number() || !std::isfinite(value.get<double>())) return false;
        radar_.on_esphome("mr60bha2/"+kind,value.dump(),measured_at);
      }
    } else if(kind=="env") { env_=data; env_at_=measured_at; }
    else if(kind=="relay") {
      if(data.value("role","")!="humidifier" || !pyget(data,"on").is_boolean() ||
         !pyget(data,"relay_seq").is_number_unsigned() || data["relay_seq"].get<unsigned long long>()>4294967295ULL)return false;
      relay_=data;relay_at_=measured_at;
      relay_sequence_=std::max(relay_sequence_,data["relay_seq"].get<unsigned long>());
    }
    else {
      last_tof_=data; tof_at_=measured_at;
      if(auto ev=tof_.on_tof(data,measured_at)) {
        publish_(topics::TOF_EVENT,Json{{"v",1},{"boot_id",boot_},{"id",boot_+":"+std::to_string(++event_seq_)},
          {"event","rail_intrusion"},{"uptime_ms",static_cast<long long>((now-start_)*1000)},
          {"distance_mm",ev->distance_mm},{"baseline_mm",ev->baseline_mm}});
      }
    }
    return true;
  } catch(const Json::exception&) { return false; }
}
void SensorHub::tick(double now,double wall) {
  const auto sequence=++seq_;
  const auto stamp=[&](Json& msg) { msg["boot_id"]=boot_; msg["seq"]=sequence; msg["source"]="pi4-usb"; msg["ts"]=wall; };
  auto radar=radar_.step(now).value_or(Json::object());
  radar.erase("raw_breath");radar.erase("heart_rate");radar.erase("distance");
  radar["sample_id"]=sources_["radar"].boot+":"+py_str(pyget(radar,"sample_id",0));
  stamp(radar); publish_(topics::RADAR,radar);
  Json env=env_.is_object()?env_:Json::object();
  env["valid"]=env_at_>=0 && now-env_at_<=20 && truthy(pyget(env,"valid",true));
  env["age_s"]=env_at_<0?Json(nullptr):Json(now-env_at_);
  stamp(env); publish_(topics::ENV,env);
  const bool relay_live=relay_at_>=0 && now-relay_at_<=3;
  publish_("control/humidifier/state",Json{{"v",1},{"role","humidifier"},
    {"usb_connected",relay_live},{"actual_on",relay_live?pyget(relay_,"on"):Json(nullptr)},
    {"command_id",relay_live?pyget(relay_,"command_id"):Json(nullptr)},
    {"reason",relay_live?pyget(relay_,"reason"):Json("waiting_for_relay")},{"ts",wall}});
  publish_(topics::PI4_STATUS,Json{{"v",1},{"boot_id",boot_},{"seq",sequence},
    {"uptime_ms",static_cast<long long>((now-start_)*1000)}, {"ts",wall},
    {"env_valid",env["valid"]},{"radar_valid",pyget(radar,"valid",false)},
    {"tof_valid",tof_at_>=0 && now-tof_at_<=3 && truthy(pyget(last_tof_,"valid"))},
    {"tof_baseline_mm",tof_.baseline()}});
}
std::optional<std::string> SensorHub::humidifier_command(const Json& command,double now,bool retained){
  if(retained || !command.is_object())return std::nullopt;
  try{
    if(command.value("v",0)!=1 || command.value("role","")!="humidifier" ||
       command.value("mode","")!="local_relay" || !pyget(command,"on").is_boolean())return std::nullopt;
    const auto id=command.value("id","");const bool on=command["on"].get<bool>();
    if(id.empty() || id.size()>100 || relay_seen_.count(id))return std::nullopt;
    const auto source=sources_.find("env");
    if(source==sources_.end() || source->second.boot.empty() || relay_at_<0 || now-relay_at_>3)return std::nullopt;
    if(on && (env_at_<0 || now-env_at_>20 || !truthy(pyget(env_,"valid",true)) ||
       pyget(command,"sample_id")!=pyget(env_,"sample_id")))return std::nullopt;
    relay_seen_.insert(id);relay_recent_.push_back(id);
    if(relay_recent_.size()>128){relay_seen_.erase(relay_recent_.front());relay_recent_.pop_front();}
    Json frame{{"v",1},{"role","humidifier"},{"boot_id",source->second.boot},
      {"seq",++relay_sequence_},{"command_id",id},{"on",on},{"lease_ms",4000}};
    return "NUNIH1 "+frame.dump();
  }catch(const Json::exception&){return std::nullopt;}
}
void SootheGate::heartbeat(const Json& status,double now) {
  if(!status.is_object() || !pyget(status,"boot_id").is_string() || !pyget(status,"uptime_ms").is_number()) return;
  if(status_.is_object() && pyget(status,"boot_id")==pyget(status_,"boot_id") &&
     num_or(pyget(status,"uptime_ms"),0)<=num_or(pyget(status_,"uptime_ms"),0))return;
  status_=status; at_=now;
}
bool SootheGate::accept(const Json& event,double now,bool retained) {
  if(retained || at_<0 || now<at_ || now-at_>3 || !event.is_object()) return false;
  try {
    if(event.value("boot_id","")!=status_.value("boot_id","") || event.value("event","")!="rail_intrusion") return false;
    const auto id=event.value("id","");
    if(id.empty() || seen_.count(id) || !pyget(event,"uptime_ms").is_number()) return false;
    const double age=num_or(pyget(status_,"uptime_ms"),0)-num_or(pyget(event,"uptime_ms"),0)+(now-at_)*1000;
    if(age < -2000 || age>5000 || num_or(pyget(event,"baseline_mm"),-1)<300) return false;
    seen_.insert(id); order_.push_back(id);
    if(order_.size()>512) { seen_.erase(order_.front()); order_.pop_front(); }
    return true;
  } catch(const Json::exception&) { return false; }
}
}
