#include "nuni/personalization.hpp"

#include <algorithm>
#include <cmath>

namespace nuni {

void Personalizer::update(std::optional<double> breathing_rate, std::optional<double> movement) {
  n_ += 1;
  // Python: `if breathing_rate and breathing_rate > 0` — None·0은 건너뛴다(무호흡은 기준선에서 제외)
  if (breathing_rate && *breathing_rate != 0.0 && *breathing_rate > 0) {
    if (!br_mean_) {
      br_mean_ = *breathing_rate;
    } else {
      const double d = *breathing_rate - *br_mean_;
      *br_mean_ += alpha_ * d;
      br_var_ = (1 - alpha_) * (br_var_ + alpha_ * d * d);
    }
  }
  if (movement) {
    if (!move_mean_) move_mean_ = *movement;
    else *move_mean_ = *move_mean_ + alpha_ * (*movement - *move_mean_);
  }
}

std::optional<std::pair<double, double>> Personalizer::normal_bpm_range(double k) const {
  if (!br_mean_) return std::nullopt;
  const double sd = std::sqrt(std::max(br_var_, 0.0));
  return std::make_pair(py_round(*br_mean_ - k * sd, 1), py_round(*br_mean_ + k * sd, 1));
}

double Personalizer::restless_threshold(double factor, double floor) const {
  const double base = move_mean_ ? *move_mean_ : 0.1;
  return py_round(std::max(floor, base * factor + 0.2), 2);
}

Json Personalizer::summary() const {
  Json out;
  out["samples"] = n_;
  out["mode"] = "observation_only";
  out["sample_definition"] = "unique_valid_quiet_breath";
  out["bpm_mean"] = br_mean_ ? Json(py_round(*br_mean_, 1)) : Json(nullptr);
  auto r = normal_bpm_range();
  out["bpm_normal_range"] = r ? Json::array({r->first, r->second}) : Json(nullptr);
  out["restless_threshold"] = restless_threshold();
  return out;
}

}  // namespace nuni
