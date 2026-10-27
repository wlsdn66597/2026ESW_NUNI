#include <cassert>
#include <iostream>
#include <vector>
#include "nuni/fusion.hpp"
#include "nuni/sensor_hub.hpp"
#include "nuni/topics.hpp"
using namespace nuni;
Json radar(){return Json{{"valid",true},{"presence",true},{"presence_valid",true},{"breath_valid",true},
  {"breathing_rate",30},{"movement",0.1},{"movement_valid",true},{"apnea",false},{"apnea_valid",true},
  {"boot_id","hub-a"},{"seq",1},{"sample_id","breath-1"}};}
int main(){
  const Json crying{{"is_crying",true},{"confidence",0.9},{"cls","cry"}};
  auto r=radar();r["breath_valid"]=false;r["breathing_rate"]=0;
  assert(decide(r,crying,nullptr).level=="attention");
  r["presence"]=false;r["movement"]=1;
  assert(decide(r,crying,nullptr).level=="attention");
  r=radar();r["breathing_rate"]=7;
  assert(decide(r,nullptr,nullptr).level=="alert");
  r["breath_valid"]=false;r["apnea"]=true;
  assert(decide(r,nullptr,nullptr).level=="alert");
  r["apnea_valid"]=false;
  assert(decide(r,nullptr,nullptr).level=="normal");
  std::cout<<"PASS missing respiration, independent valid alarm\n";
  r=radar();r["movement"]=0.9;
  assert(decide(r,crying,nullptr).level=="alert");
  assert(single_sensor_decide(r,crying,nullptr).level=="attention");

  double time=100;std::vector<Json> control;
  Fusion f([&](const auto& topic,const auto& p){if(topic==topics::CONTROL)control.push_back(p);},2,10,[&]{return time;});
  f.on_message(topics::RADAR,radar());
  for(int i=0;i<100;++i)f.on_message(topics::ENV,Json{{"co2",500},{"temp",22}});
  assert(f.last_state()["personal"]["samples"]==1);
  r=radar();r["seq"]=2; // same original breath, new heartbeat
  f.on_message(topics::RADAR,r);
  assert(f.last_state()["personal"]["samples"]==1);
  r["seq"]=1;r["sample_id"]="old";
  f.on_message(topics::RADAR,r);
  assert(f.last_state()["personal"]["samples"]==1);
  f.on_message(topics::CRY,crying);
  for(int i=0;i<100;++i)f.tick();
  assert(f.last_state()["level"]=="normal");
  time=102;f.tick();assert(f.last_state()["level"]=="attention");
  time=104;f.tick();assert(f.current_sensors()["radar"].is_null());assert(f.current_sensors()["cry"].is_null());
  time=121;f.tick();assert(f.current_sensors()["env"].is_null());
  assert(control.back()["actuation_enabled"]==false);
  for(const auto& v:control.back()["desired"].items())assert(v.value()==false);
  std::cout<<"PASS duplicate statistics, elapsed debounce, total silence, recommendation OFF\n";

  RadarBridge bridge;
  bridge.on_esphome("mr60bha2/presence","ON",100);
  bridge.on_esphome("mr60bha2/distance","60",100);
  bridge.on_esphome("mr60bha2/distance","60",101);
  bridge.on_esphome("mr60bha2/breath_rate","30",101);
  assert((*bridge.step(101))["breath_valid"]==true);
  for(int t=102;t<=112;++t){
    bridge.on_esphome("mr60bha2/presence","ON",t);
    bridge.on_esphome("mr60bha2/distance","60",t);
    bridge.step(t);
  }
  const auto stale=*bridge.step(112);
  assert(stale["breath_valid"]==false && stale["apnea"]==false && stale["presence_valid"]==true);
  std::cout<<"PASS partial UART field failure does not refresh respiration\n";

  TofWatch w;
  auto tof=[](int distance,bool valid=true){return Json{{"distance_mm",distance},{"valid",valid}};};
  for(int i=0;i<5;++i)assert(!w.on_tof(tof(700),i));
  assert(w.baseline()==700);
  for(int i=0;i<30;++i)assert(!w.on_tof(tof(400),5+i/10.0));
  assert(w.on_tof(tof(400),8));
  for(int i=9;i<=14;++i)assert(!w.on_tof(tof(700),i));
  for(int i=15;i<=19;++i)assert(!w.on_tof(tof(400),i));
  TofWatch no_baseline;
  for(int i=0;i<10;++i)assert(!no_baseline.on_tof(tof(-1,false),i));
  for(int i=10;i<30;++i)assert(!no_baseline.on_tof(tof(100),i));
  assert(no_baseline.baseline()==-1);
  TofWatch interrupted;
  for(int i=0;i<5;++i)interrupted.on_tof(tof(700),i);
  interrupted.on_tof(tof(400),5);interrupted.on_tof(tof(-1,false),6);
  assert(!interrupted.on_tof(tof(400),7));assert(!interrupted.on_tof(tof(400),8));
  assert(!interrupted.on_tof(tof(400),9));assert(interrupted.on_tof(tof(400),10));
  std::cout<<"PASS ToF elapsed time, cooldown, invalid reset, no baseline no trigger\n";

  std::vector<Json> output;
  SensorHub hub([&](const auto&,const auto& j){output.push_back(j);},"hub",0);
  auto frame=[](const std::string& boot,int seq){return "[I] NUNI1 "+Json{{"v",1},{"device","env"},{"boot_id",boot},{"seq",seq},
    {"uptime_ms",seq*1000},{"kind","tof"},{"data",Json{{"valid",true},{"distance_mm",700}}}}.dump()+"\x1b[0m";};
  assert(hub.on_line("env",frame("esp-a",1),1));
  assert(!hub.on_line("env",frame("esp-a",1),2));
  assert(!hub.on_line("radar",frame("esp-a",2),2));
  assert(hub.on_line("env",frame("esp-b",1),3));
  assert(!hub.on_line("env",frame("esp-a",3),4));
  assert(!hub.on_line("env","NUNI1 {broken}",5));
  SootheGate gate;
  Json status{{"boot_id","hub"},{"uptime_ms",1000}};
  Json event{{"boot_id","hub"},{"id","hub:1"},{"uptime_ms",1100},{"event","rail_intrusion"},{"baseline_mm",700}};
  assert(!gate.accept(event,1));gate.heartbeat(status,1);
  assert(!gate.accept(event,1.1,true));assert(gate.accept(event,1.1));assert(!gate.accept(event,1.2));
  event["id"]="hub:2";assert(!gate.accept(event,5));
  gate.heartbeat(status,6);event["boot_id"]="old-hub";assert(!gate.accept(event,6));
  std::cout<<"PASS USB framing, reboot/reorder, retained/duplicate/stale event rejection\n";
}
