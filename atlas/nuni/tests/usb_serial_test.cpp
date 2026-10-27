#include <cassert>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include "nuni/topics.hpp"
#include "nuni/usb_serial.hpp"

using namespace nuni;

int main() {
  const int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
  assert(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
  const std::string path = ptsname(master);
  Json latest;
  SensorHub hub([&](const auto& topic, const auto& value) {
    if (topic == topics::ENV) latest = value;
  }, "test-hub", 100);
  UsbSerialPort port(path, "env", 15);
  auto frame = [](int seq, int uptime, int co2) {
    return "NUNI1 " + Json{{"v", 1}, {"device", "env"}, {"boot_id", "esp-test"},
      {"seq", seq}, {"uptime_ms", uptime}, {"kind", "env"},
      {"data", Json{{"valid", true}, {"temp", 22}, {"co2", co2}}}}.dump() + "\n";
  };
  auto send = [&](const std::string& bytes) {
    assert(::write(master, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  };

  port.read(hub, 100);
  assert(port.is_open());
  assert(port.write_line("NUNIH1 {\"v\":1}"));
  char outgoing[128]{};ssize_t outgoing_size=-1;
  for(int attempt=0;attempt<20 && outgoing_size<0;++attempt){outgoing_size=::read(master,outgoing,sizeof(outgoing));if(outgoing_size<0)usleep(1000);}
  assert(outgoing_size>0 && std::string(outgoing,static_cast<size_t>(outgoing_size))=="NUNIH1 {\"v\":1}\n");
  assert(!port.write_line("bad\nframe"));
  const auto first = frame(1, 1000, 500);
  send(first.substr(0, first.size() / 2)); port.read(hub, 101);
  hub.tick(101, 1001); assert(latest["valid"] == false);
  send(first.substr(first.size() / 2)); port.read(hub, 102);
  hub.tick(102, 1002); assert(latest["valid"] == true && latest["co2"] == 500);

  // The USB descriptor is still present, but bytes cease without a hangup.
  port.read(hub, 116.9); assert(port.is_open());
  port.read(hub, 117); assert(!port.is_open());
  port.read(hub, 118); assert(!port.is_open());
  port.read(hub, 119); assert(port.is_open());

  // Reopening must not turn a replayed old sample into fresh environment data.
  send(first); port.read(hub, 123);
  hub.tick(123, 1023); assert(latest["valid"] == false);
  send(frame(2, 23000, 550)); port.read(hub, 124);
  hub.tick(124, 1024); assert(latest["valid"] == true && latest["co2"] == 550);

  close(master); port.read(hub, 125); assert(!port.is_open());
  std::cout << "PASS PTY split frames, silent USB recovery, retry delay, stale replay rejection, resumed measurements, disconnect\n";
}
