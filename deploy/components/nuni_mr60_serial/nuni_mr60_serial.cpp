// MR60 wire format follows ESPHome's official seeed_mr60bha2 component
// (MIT, ESPHome contributors). Unlike that UI component, zero BPM frames are
// forwarded too, and unchanged presence reports are not deduplicated.
#include "nuni_mr60_serial.h"
#include "esphome/core/log.h"
#include <esp_random.h>
#include <cmath>
#include <cstring>
namespace esphome::nuni_mr60_serial {
namespace {
uint8_t checksum(const uint8_t* p,size_t n){uint8_t c=0;for(size_t i=0;i<n;++i)c^=p[i];return ~c;}
uint16_t be16(const uint8_t* p){return uint16_t(p[0])<<8|p[1];}
}
void SerialRadar::setup(){boot_=esp_random();bytes_.reserve(521);}
void SerialRadar::consume(uint8_t b){
  if(bytes_.empty()&&b!=1)return;
  bytes_.push_back(b);
  if(bytes_.size()<8)return;
  const size_t len=be16(bytes_.data()+3);
  if(len>512 || checksum(bytes_.data(),7)!=bytes_[7]){bytes_.clear();return;}
  if(bytes_.size()<len+9)return;
  if(checksum(bytes_.data()+8,len)==bytes_[8+len])frame(be16(bytes_.data()+5),bytes_.data()+8,len);
  bytes_.clear();
}
void SerialRadar::loop(){
  uint8_t b;for(size_t count=0;count<4096 && available();++count)if(read_byte(&b))consume(b);
}
void SerialRadar::frame(uint16_t type,const uint8_t* d,size_t len){
  const char* kind=nullptr;float value=0;
  if(type==0x0F09 && len>=2){
    ESP_LOGI("nuni.serial","NUNI1 {\"v\":1,\"device\":\"radar\",\"boot_id\":\"%08lx\",\"seq\":%lu,\"uptime_ms\":%lu,\"kind\":\"presence\",\"data\":{\"value\":%s}}",
      (unsigned long)boot_,(unsigned long)++seq_,(unsigned long)millis(),(d[0]|d[1])?"true":"false");return;
  }
  if(type==0x0A14 && len>=4){kind="breath_rate";memcpy(&value,d,4);}
  if(type==0x0A15 && len>=4){kind="heart_rate";memcpy(&value,d,4);}
  if(type==0x0A16 && len>=8 && d[0]){kind="distance";memcpy(&value,d+4,4);}
  if(!kind || !std::isfinite(value) || value<0)return;
  ESP_LOGI("nuni.serial","NUNI1 {\"v\":1,\"device\":\"radar\",\"boot_id\":\"%08lx\",\"seq\":%lu,\"uptime_ms\":%lu,\"kind\":\"%s\",\"data\":{\"value\":%.4f}}",
    (unsigned long)boot_,(unsigned long)++seq_,(unsigned long)millis(),kind,value);
}
}
