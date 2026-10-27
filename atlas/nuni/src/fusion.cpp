#include "nuni/fusion.hpp"
#include "nuni/env_control.hpp"
#include "nuni/sleep_state.hpp"
#include "nuni/topics.hpp"
#include <cmath>
namespace nuni {
namespace {
int order(const std::string& s){return s=="alert"?2:s=="attention"?1:0;}
std::string max_level(const std::string& a,const std::string& b){return order(a)>=order(b)?a:b;}
std::string join(const std::vector<std::string>& v,const char* sep){std::string s;for(const auto& x:v){if(!s.empty())s+=sep;s+=x;}return s;}
bool usable(const Json& j){return j.is_object() && !j.empty() && truthy(pyget(j,"valid",true));}
}
Decision decide(const Json& radar,const Json& cry,const Json& env){
  std::string level="normal";std::vector<std::string> reasons;
  const bool present=usable(radar) && truthy(pyget(radar,"presence")) && truthy(pyget(radar,"presence_valid",true));
  const double br=num_or(pyget(radar,"breathing_rate",pyget(radar,"breathing_bpm")),0);
  const bool br_valid=present && truthy(pyget(radar,"breath_valid",false)) && std::isfinite(br) && br>0;
  const bool apnea=present && truthy(pyget(radar,"apnea_valid",false)) && truthy(pyget(radar,"apnea"));
  const Json assessment=pyget(radar,"assessment");
  const bool abnormal=present && (assessment.is_object()?truthy(pyget(assessment,"breath_abnormal")):(apnea || (br_valid && br<8)));
  const bool motion=present && truthy(pyget(radar,"movement_valid",false)) &&
    (assessment.is_object()?truthy(pyget(assessment,"movement_high")):num_or(pyget(radar,"movement"),0)>0.6);
  const bool borderline=br_valid && (assessment.is_object()?truthy(pyget(assessment,"borderline")):((br>=8 && br<=15)||br>55));
  if(abnormal){level="alert";reasons.emplace_back("호흡 신호 이상 확인 필요");}
  const bool crying=usable(cry) && truthy(pyget(cry,"is_crying")) && num_or(pyget(cry,"confidence",pyget(cry,"cry_score")),0)>=0.7;
  if(crying){level=max_level(level,"attention");reasons.emplace_back("울음 감지("+py_str(pyget(cry,"cls"))+")");}
  if(crying && (abnormal || motion)){level="alert";reasons.emplace_back("다중 신호 동시 이상");}
  if(borderline && (crying || (usable(env)&&num_or(pyget(env,"co2"),0)>1000))){level="alert";reasons.emplace_back("경계 호흡+보강신호 교차검증");}
  if(usable(env)){
    if(num_or(pyget(env,"co2"),0)>1000){level=max_level(level,"attention");reasons.emplace_back("CO₂ 높음");}
    auto t=as_number(pyget(env,"temp"));if(t&&std::isfinite(*t)&&(*t<18||*t>26)){level=max_level(level,"attention");reasons.emplace_back("실내 온도 이상");}
  }
  return {level,reasons};
}
Decision single_sensor_decide(const Json& radar,const Json& cry,const Json& env){
  Decision result{"normal",{}};
  for(const auto& d:{decide(radar,nullptr,nullptr),decide(nullptr,cry,nullptr),decide(nullptr,nullptr,env)}){
    result.level=max_level(result.level,d.level);
    result.reasons.insert(result.reasons.end(),d.reasons.begin(),d.reasons.end());
  }
  return result;
}
Fusion::Fusion(PublishFn publish,int debounce_n,double cooldown_s,NowFn now)
  :publish_(std::move(publish)),now_(std::move(now)),debounce_n_(debounce_n),cooldown_s_(cooldown_s){}
bool Fusion::accept(const std::string& topic,const Json& payload){
  if(!payload.is_object())return false;
  try {
    if(payload.contains("boot_id") && payload.contains("seq")){
      const auto boot=payload.at("boot_id").get<std::string>();const auto seq=payload.at("seq").get<long long>();
      auto it=sequences_.find(topic);
      if(it!=sequences_.end()){
        if(it->second.first==boot && seq<=it->second.second)return false;
        if(it->second.first!=boot){if(retired_[topic].count(boot))return false;retired_[topic].insert(it->second.first);}
      }
      sequences_[topic]={boot,seq};
    }
    received_[topic]=now_();return true;
  }catch(const Json::exception&){return false;}
}
Json Fusion::fresh(const std::string& topic,const Json& payload,double ttl)const{
  const auto it=received_.find(topic);
  if(it==received_.end() || now_()<it->second || now_()-it->second>ttl || !usable(payload))return nullptr;
  return payload;
}
Json Fusion::current_sensors()const{return Json{{"radar",fresh(topics::RADAR,radar_,3)},
  {"cry",fresh(topics::CRY,cry_,3)},{"env",fresh(topics::ENV,env_,20)}};}
std::string Fusion::debounce(const std::string& level){
  if(level=="normal"){candidate_="normal";candidate_since_=-1;return level;}
  if(level!=candidate_ || candidate_since_<0){candidate_=level;candidate_since_=now_();}
  return now_()-candidate_since_>=debounce_n_?level:"normal";
}
bool Fusion::cooldown_allows(const std::string& level,const std::vector<std::string>& reasons){
  const auto key=level+'\x1f'+join(reasons,"|");const auto now=now_();
  if(has_last_alert_ && key==last_alert_key_ && now-last_alert_ts_<cooldown_s_)return false;
  has_last_alert_=true;last_alert_key_=key;last_alert_ts_=now;return true;
}
Fusion::Result Fusion::process(){
  const auto sensors=current_sensors();const auto r=sensors["radar"],c=sensors["cry"],e=sensors["env"];
  const auto decision=decide(r,c,e);const auto level=debounce(decision.level);
  const auto reasons=level=="normal"?std::vector<std::string>{}:decision.reasons;
  const auto sleep=sleep_state::classify(r,c);const auto actions=env_control::recommend(e,sleep.state);
  Json acts=Json::array(),desired=Json::object();
  for(const char* a:{"ventilate","humidify","dehumidify","cool","heat","dim_light"})desired[a]=false;
  for(const auto& a:actions){acts.push_back(Json::array({a.first,a.second}));desired[a.first]=true;}
  // A recommendation is never a hardware command. All OFF targets are explicit;
  // no actuator has been installed/acknowledged by this application.
  publish_(topics::CONTROL,Json{{"schema",2},{"mode","recommendation_only"},{"desired",desired},
    {"actuation_enabled",false},{"actual",nullptr},{"ts",wall_now()}});
  Json state{{"level",level},{"raw_level",decision.level},{"reasons",reasons},
    {"sleep",Json{{"state",sleep.state},{"reason",sleep.reason}}},{"personal",personalizer_.summary()},
    {"actions",acts},{"quality",Json{{"radar",!r.is_null()},{"cry",!c.is_null()},{"env",!e.is_null()}}},
    {"ts",wall_now()}};
  last_state_=state;publish_(topics::FUSION_STATE,state);
  bool emitted=false;
  if(level!="normal" && level!=last_ && cooldown_allows(level,reasons)){
    publish_(topics::ALERT,Json{{"level",level},{"reason",join(reasons,", ")},{"ts",wall_now()}});emitted=true;
  }
  last_=level;return {level,reasons,emitted};
}
Fusion::Result Fusion::on_message(const std::string& topic,const Json& payload){
  if((topic==topics::RADAR || topic==topics::CRY || topic==topics::ENV) && accept(topic,payload)){
    if(topic==topics::RADAR){
      radar_=payload;
      const auto id=payload.contains("sample_id")?payload["sample_id"].dump():pyget(payload,"ts").dump();
      const auto br=as_number(pyget(payload,"breathing_rate"));
      if(id!="null" && id!=last_personal_id_ && usable(payload) && truthy(pyget(payload,"presence")) &&
         truthy(pyget(payload,"breath_valid",false)) && truthy(pyget(payload,"movement_valid",false)) &&
         br && std::isfinite(*br) && *br>=8 && *br<=55 && num_or(pyget(payload,"movement"),1)<=0.4){
        personalizer_.update(br,as_number(pyget(payload,"movement")));last_personal_id_=id;
      }
    } else if(topic==topics::CRY)cry_=payload;else env_=payload;
  }
  return process();
}
Fusion::Result Fusion::update(const Json& radar,const Json& cry,const Json& env){
  if(!radar.is_null())on_message(topics::RADAR,radar);
  if(!cry.is_null())on_message(topics::CRY,cry);
  if(!env.is_null())on_message(topics::ENV,env);
  return process();
}
Fusion::Result Fusion::tick(){return process();}
}
