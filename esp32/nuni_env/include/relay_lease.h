#pragma once
#include <stdint.h>
class RelayLease {
 public:
  bool accept(uint32_t seq,bool on,uint32_t duration,uint32_t now){
    if(seq==0 || seq<=sequence_ || duration<500 || duration>4000)return false;
    sequence_=seq;active_=on;started_=now;duration_=duration;return true;
  }
  bool tick(uint32_t now){
    if(active_ && uint32_t(now-started_)>=duration_){active_=false;return true;}
    return false;
  }
  bool on()const{return active_;}
  uint32_t sequence()const{return sequence_;}
 private:
  bool active_=false;
  uint32_t sequence_=0,started_=0,duration_=0;
};
