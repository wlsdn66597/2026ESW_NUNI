#pragma once
#include <functional>
#include <map>
#include <set>
#include <deque>
#include "nuni/radar_bridge.hpp"
#include "nuni/tof_watch.hpp"
namespace nuni {
class SensorHub {
 public:
  using Publish = std::function<void(const std::string&,const Json&)>;
  SensorHub(Publish publish, std::string boot, double start, TofWatchConfig tof = {});
  bool on_line(const std::string& channel, const std::string& line, double now);
  void tick(double now, double wall);
  std::optional<std::string> humidifier_command(const Json& command, double now, bool retained=false);
 private:
  struct Source { std::string boot; long long seq=-1; std::set<std::string> retired; double offset=0; };
  Publish publish_;
  std::string boot_;
  double start_, env_at_=-1, tof_at_=-1;
  unsigned long seq_=0, event_seq_=0;
  std::map<std::string,Source> sources_;
  RadarBridge radar_;
  TofWatch tof_;
  Json env_, last_tof_;
  Json relay_;
  double relay_at_=-1;
  unsigned long relay_sequence_=0;
  std::set<std::string> relay_seen_;
  std::deque<std::string> relay_recent_;
};
class SootheGate {
 public:
  void heartbeat(const Json& status,double now);
  bool accept(const Json& event,double now,bool retained=false);
 private:
  Json status_;
  double at_=-1;
  std::set<std::string> seen_;
  std::deque<std::string> order_;
};
}
