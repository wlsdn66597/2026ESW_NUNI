// Isolated session-bus test server. Never installed on a device.
#include "nuni/history_store.hpp"
#include "nuni/dbus_service.hpp"
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>
namespace { volatile std::sig_atomic_t stop=0; void interrupted(int){stop=1;} }
int main(int argc,char** argv){
  if(argc!=3)return 2;
  try {
    std::signal(SIGINT,interrupted);std::signal(SIGTERM,interrupted);
    nuni::HistoryStore::Config cfg;cfg.path=argv[1];nuni::HistoryStore history(cfg);
    std::ifstream input(argv[2]);if(!input)throw std::runtime_error("cannot read fixture");
    std::string line;
    while(std::getline(input,line)){
      const auto row=nuni::Json::parse(line);
      if(!history.record(row.at("topic").get<std::string>(),row.at("payload"),row.at("at").get<double>()))
        throw std::runtime_error("fixture rejected");
    }
    nuni::DbusHandlers h;
    h.get_environment_history=[&](int64_t from,int64_t to,uint32_t limit){return history.query(from,to,limit).dump();};
    h.get_history_summary=[&](int64_t from,int64_t to,uint32_t limit){return history.summary(from,to,limit).dump();};
    h.get_sensor_history=[&](int64_t from,int64_t to,uint32_t limit){return history.sensor_history(from,to,limit).dump();};
    h.get_recorded_events=[&](int64_t from,int64_t to,uint32_t limit,int64_t after){return history.events(from,to,limit,after).dump();};
    auto service=nuni::make_dbus_service(false,std::move(h));
    if(!service)throw std::runtime_error("cannot start session D-Bus service");
    std::cout<<"ready\n"<<std::flush;
    while(!stop)std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}
}
