// Pi 4: two USB serial ports -> local processing -> Pi 5 over direct Ethernet.
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#include "nuni/clock.hpp"
#include "nuni/mqtt.hpp"
#include "nuni/sensor_hub.hpp"
#include "nuni/usb_serial.hpp"
using namespace nuni;
namespace {
volatile sig_atomic_t stop=0;
void halt(int) { stop=1; }
std::string env(const char* k,const char* fallback) { auto p=std::getenv(k); return p?p:fallback; }

}
int main() {
  signal(SIGINT,halt);signal(SIGTERM,halt);signal(SIGPIPE,SIG_IGN);
  const double start=steady_now();
  std::ifstream bootfile("/proc/sys/kernel/random/boot_id"); std::string boot;bootfile>>boot;
  boot+="-"+std::to_string(getpid())+"-"+std::to_string(static_cast<long long>(start*1000));
  MqttClient mqtt(env("NUNI_MQTT_HOST","10.77.0.5"),1883,"nunisensors-"+boot);
  TofWatchConfig tc;
  auto number=[](const char* k,double d){auto p=std::getenv(k);return p?std::strtod(p,nullptr):d;};
  tc.drop_mm=number("NUNI_TOF_DROP_MM",150);tc.hold_s=number("NUNI_TOF_HOLD_S",3);
  tc.clear_s=number("NUNI_TOF_CLEAR_S",5);tc.cooldown_s=number("NUNI_SOOTHE_COOLDOWN",180);
  SensorHub hub([&](const auto& topic,const auto& payload){mqtt.publish(topic,dump(payload));},boot,start,tc);
  UsbSerialPort env_port{env("NUNI_ENV_PORT","/dev/nuni-env"),"env",15};
  UsbSerialPort radar_port{env("NUNI_RADAR_PORT","/dev/nuni-radar"),"radar"};
  mqtt.subscribe("control/humidifier/command");
  mqtt.set_on_message([&](const std::string& topic,const std::string& raw){
    if(topic!="control/humidifier/command")return;
    if(auto command=parse_payload(raw)){
      if(auto line=hub.humidifier_command(*command,steady_now(),mqtt.message_retained()))
        env_port.write_line(*line);
    }
  });
  double tick=start;
  while(!stop) {
    env_port.read(hub,steady_now());
    radar_port.read(hub,steady_now());
    const auto now=steady_now();
    if(now>=tick) {hub.tick(now,wall_now());tick=now+1;}
    mqtt.poll(20);
  }
}
