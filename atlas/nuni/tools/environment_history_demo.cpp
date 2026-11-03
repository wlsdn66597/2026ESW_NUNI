#include "nuni/history_store.hpp"
#include <chrono>
#include <iostream>
int main(int argc,char** argv) {
  try {
    nuni::HistoryStore::Config c;
    c.path=argc>1 ? argv[1] : "environment-demo.db";
    const int64_t now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t minute=now/60*60;
    {
      nuni::HistoryStore h(c);
      for(int i=0;i<3;++i) h.observe(nuni::Json{{"sample_id","demo-"+std::to_string(minute)+":"+std::to_string(i)},
        {"source","pi4-usb"},{"valid",true},{"age_s",0},{"temp",23+i},{"humidity",45},{"co2",650}},now);
    }
    nuni::HistoryStore reopened(c);
    const auto result=reopened.query(minute,minute+60);
    std::cout<<result.dump(2)<<"\n";
    return result.value("ok",false) ? 0 : 1;
  } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
