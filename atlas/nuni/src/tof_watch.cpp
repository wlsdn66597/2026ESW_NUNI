#include "nuni/tof_watch.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
namespace nuni {
void TofWatch::reset() {
  learn_.clear(); baseline_.reset(); occupied_since_.reset(); clear_since_.reset();
  last_msg_.reset();last_valid_.reset();armed_ = true;
}
std::optional<TofWatch::Event> TofWatch::on_tof(const Json& msg, double now) {
  if (last_msg_ && now <= *last_msg_) return std::nullopt;
  if (last_msg_ && now - *last_msg_ > c_.relearn_s) reset();
  if (last_msg_ && now - *last_msg_ > c_.stale_s) { occupied_since_.reset(); clear_since_.reset(); learn_.clear(); }
  last_msg_ = now;
  const auto distance = as_number(pyget(msg, "distance_mm"));
  const bool valid = truthy(pyget(msg,"valid")) && distance && std::isfinite(*distance) && *distance > 0;
  if (!valid) { occupied_since_.reset(); clear_since_.reset(); learn_.clear(); return std::nullopt; }
  const double d = *distance;
  if(last_valid_ && now-*last_valid_>c_.relearn_s) {
    reset();last_msg_=now;
  }
  last_valid_=now;
  if (!baseline_) {
    if (d < c_.min_baseline_mm) { learn_.clear(); return std::nullopt; }
    learn_.push_back(d);
    while (learn_.size() > static_cast<size_t>(c_.learn_n)) learn_.pop_front();
    if (learn_.size() == static_cast<size_t>(c_.learn_n)) {
      std::vector<double> sorted(learn_.begin(), learn_.end()); std::sort(sorted.begin(), sorted.end());
      if (sorted.back() - sorted.front() <= c_.stable_span_mm) baseline_ = sorted[sorted.size()/2];
    }
    return std::nullopt;
  }
  if (d >= *baseline_ - c_.drop_mm) {
    occupied_since_.reset();
    if (!clear_since_) clear_since_ = now;
    if (now - *clear_since_ >= c_.clear_s) armed_ = true;
    return std::nullopt;
  }
  clear_since_.reset();
  if (!occupied_since_) occupied_since_ = now;
  if (!armed_ || now - *occupied_since_ < c_.hold_s ||
      (last_fire_ && now - *last_fire_ < c_.cooldown_s)) return std::nullopt;
  armed_ = false; last_fire_ = now;
  return Event{d,*baseline_};
}
}
