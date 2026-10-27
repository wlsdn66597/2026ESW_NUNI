#pragma once
#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include <vector>
namespace esphome::nuni_mr60_serial {
class SerialRadar : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
 private:
  void consume(uint8_t byte);
  void frame(uint16_t type,const uint8_t* data,size_t len);
  std::vector<uint8_t> bytes_;
  uint32_t boot_=0, seq_=0;
};
}
