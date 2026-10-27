#include <cassert>
#include <iostream>
#include "nuni/sensor_hub.hpp"
#include "relay_lease.h"
using namespace nuni;
int main(){
  Json state;
  SensorHub hub([&](const auto&t,const auto&j){if(t=="control/humidifier/state")state=j;},"hub",100);
  auto frame=[](int seq,const char*kind,Json data){return "NUNI1 "+Json{{"v",1},{"device","env"},
    {"boot_id","env-boot"},{"seq",seq},{"uptime_ms",seq*1000},{"kind",kind},{"data",data}}.dump();};
  Json cmd{{"v",1},{"role","humidifier"},{"mode","local_relay"},{"id","on-1"},
    {"on",true},{"sample_id","env-boot:2"}};
  assert(!hub.humidifier_command(cmd,100));
  assert(hub.on_line("env",frame(1,"relay",Json{{"role","humidifier"},{"on",false},{"relay_seq",4u}}),101));
  assert(hub.on_line("env",frame(2,"env",Json{{"valid",true},{"humidity",30}}),102));
  assert(!hub.humidifier_command(cmd,102,true));
  auto line=hub.humidifier_command(cmd,102);assert(line);
  auto wire=Json::parse(line->substr(7));assert(wire["seq"]==5 && wire["on"]==true && wire["lease_ms"]==4000);
  assert(wire["boot_id"]=="env-boot");assert(!hub.humidifier_command(cmd,102.1));
  cmd["id"]="bad-sample";cmd["sample_id"]="old";assert(!hub.humidifier_command(cmd,102.2));
  hub.tick(102.5,1002);assert(state["usb_connected"]==true && state["actual_on"]==false);
  assert(hub.on_line("env",frame(3,"relay",Json{{"role","humidifier"},{"on",true},{"relay_seq",5u}}),103));
  hub.tick(103,1003);assert(state["actual_on"]==true);
  cmd["id"]="off-1";cmd["on"]=false;assert(hub.humidifier_command(cmd,103));
  hub.tick(107,1007);assert(state["usb_connected"]==false && state["actual_on"].is_null());
  cmd["id"]="stale";cmd["on"]=true;assert(!hub.humidifier_command(cmd,107));
  RelayLease lease;assert(!lease.on());assert(lease.accept(1,true,4000,100));
  assert(!lease.accept(1,true,4000,200));assert(!lease.tick(4099));assert(lease.tick(4100));assert(!lease.on());
  assert(!lease.accept(2,true,5000,4200));assert(lease.accept(2,true,4000,0xFFFFFF00u));
  assert(!lease.tick(0xFFFFFF00u+3999u));assert(lease.tick(0xFFFFFF00u+4000u));
  std::cout<<"PASS humidity USB command, boot binding, source sample, ACK, duplicate, stale, MCU lease wrap\n";
}
