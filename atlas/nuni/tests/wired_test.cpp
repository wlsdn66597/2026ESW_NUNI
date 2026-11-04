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
  assert(decide(r,nullptr,nullptr).level=="normal");  // 낮은 값 하나로는 경보하지 않는다
  r["breath_valid"]=false;r["apnea"]=true;
  assert(decide(r,nullptr,nullptr).level=="alert");
  r["presence"]=false;  // 무호흡 카운트 중 레이더가 대상을 놓쳐도 경보는 유지
  assert(decide(r,nullptr,nullptr).level=="alert");
  r["apnea_valid"]=false;
  assert(decide(r,nullptr,nullptr).level=="normal");
  std::cout<<"PASS missing respiration, apnea-only alarm\n";
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

  {
    RadarBridge b;
    auto feed=[&](double t,const char* br,const char* presence="ON",const char* dist="40"){
      b.on_esphome("mr60bha2/presence",presence,t);b.on_esphome("mr60bha2/distance",dist,t);
      b.on_esphome("mr60bha2/breath_rate",br,t);return *b.step(t);};
    for(int t=0;t<10;++t)feed(t,"18");
    assert(feed(10,"18")["breath_valid"]==true);
    for(int t=11;t<=17;++t){  // 실측과 같은 7초 하락: 경과 시간은 보내되 무호흡은 아니다
      auto m=feed(t,"3");
      assert(m["breath_valid"]==false && m["apnea"]==false && m["low_breath_s"].get<double>()==t-11);
      assert(m["assessment"]["level"]=="normal");
    }
    auto back=feed(18,"18");
    assert(back["low_breath_s"].is_null() && back["breath_valid"]==true);
    for(int t=19;t<=24;++t)assert(feed(t,"7")["breath_valid"]==true);  // 6~60은 받아들인다
    for(int t=25;t<30;++t)feed(t,"18");
    for(int t=30;t<50;++t)assert(feed(t,"2")["apnea"]==false);
    auto apnea=feed(50,"0");
    assert(apnea["apnea"]==true && apnea["assessment"]["level"]=="alert");
    std::cout<<"PASS low breath must last 20s; 7s module dip does not alarm\n";
  }
  {
    RadarBridge b;  // 카운트 중 대상이 사라져도 ToF 변화가 없으면 계속 센다
    auto feed=[&](double t,const char* br,const char* presence="ON",const char* dist="40"){
      b.on_esphome("mr60bha2/presence",presence,t);b.on_esphome("mr60bha2/distance",dist,t);
      b.on_esphome("mr60bha2/breath_rate",br,t);return *b.step(t);};
    for(int t=0;t<10;++t){b.on_tof(700,t);feed(t,"18");}
    for(int t=10;t<15;++t){b.on_tof(700,t);feed(t,"3");}
    for(int t=15;t<30;++t){b.on_tof(705,t);feed(t,"0","OFF","0");}
    auto lost=feed(30,"0","OFF","0");
    assert(lost["presence"]==false && lost["apnea"]==true);
    b.on_tof(400,31);  // 난간 ToF가 크게 바뀜 = 안아 올림 → 이탈로 보고 멈춘다
    auto left=feed(31,"0","OFF","0");
    assert(left["apnea"]==false && left["low_breath_s"].is_null());

    RadarBridge u;  // ToF를 모르면(무효) 계속 센다
    auto feed_u=[&](double t,const char* br,const char* presence="ON",const char* dist="40"){
      u.on_esphome("mr60bha2/presence",presence,t);u.on_esphome("mr60bha2/distance",dist,t);
      u.on_esphome("mr60bha2/breath_rate",br,t);return *u.step(t);};
    for(int t=0;t<10;++t)feed_u(t,"18");
    feed_u(10,"1");  // 대상이 보일 때 카운트가 시작된다
    for(int t=11;t<30;++t){u.on_tof(std::nullopt,t);feed_u(t,"1","OFF","0");}
    assert(feed_u(30,"1","OFF","0")["apnea"]==true);
    std::cout<<"PASS apnea count keeps going on target loss unless rail ToF changes\n";
  }
  {
    double now=200;std::vector<Json> alerts;
    Fusion g([&](const auto& topic,const auto& p){if(topic==topics::ALERT)alerts.push_back(p);},2,10,[&]{return now;});
    auto a=radar();a["apnea"]=true;a["breath_valid"]=false;
    g.on_message(topics::RADAR,a);  // 디바운스 전이라도 무호흡 시작은 바로 알린다
    assert(alerts.size()==1 && alerts[0]["apnea_onset"]==true && alerts[0]["level"]=="alert");
    now=203;a["seq"]=2;g.on_message(topics::RADAR,a);
    assert(alerts.size()==2 && alerts[1]["apnea_onset"]==false);  // 단계 전환 경보는 진동 대상이 아니다
    std::cout<<"PASS apnea onset alert is immediate and marked for vibration\n";
  }

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
