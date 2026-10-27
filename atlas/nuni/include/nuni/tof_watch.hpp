#pragma once
#include <deque>
#include <optional>
#include "nuni/json_util.hpp"
namespace nuni {
struct TofWatchConfig {
  double drop_mm = 150, hold_s = 3, clear_s = 5, cooldown_s = 180;
  double stale_s = 2.5, relearn_s = 10, min_baseline_mm = 300, stable_span_mm = 80;
  int learn_n = 5;
};
// Entry into a beam installed at rail height; does not classify posture.
class TofWatch {
 public:
  struct Event { double distance_mm, baseline_mm; };
  explicit TofWatch(TofWatchConfig c = {}) : c_(c) {}
  std::optional<Event> on_tof(const Json& msg, double now);
  double baseline() const { return baseline_.value_or(-1); }
  void reset();
 private:
  TofWatchConfig c_;
  std::deque<double> learn_;
  std::optional<double> baseline_, occupied_since_, clear_since_, last_msg_, last_fire_;
  std::optional<double> last_valid_;
  bool armed_ = true;
};
}
