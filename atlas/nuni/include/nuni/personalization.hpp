// 경량 개인화: 아기별 호흡·움직임 기준선 온라인 학습(지수 이동 평균) — personalization.py 이식.
#pragma once

#include <optional>
#include <utility>

#include "nuni/json_util.hpp"

namespace nuni {

class Personalizer {
 public:
  explicit Personalizer(double alpha = 0.05) : alpha_(alpha) {}

  // breathing_rate: 없음(None)이면 nullopt. 0(무호흡)은 기준선에서 뺀다.
  // movement: 없음(None)이면 nullopt.
  void update(std::optional<double> breathing_rate, std::optional<double> movement);

  // 개인 호흡 정상 범위 (mean ± k·sd), 아직 값이 없으면 nullopt.
  std::optional<std::pair<double, double>> normal_bpm_range(double k = 2.0) const;

  // 개인 움직임 기준선 기반 뒤척임 임계값.
  double restless_threshold(double factor = 2.0, double floor = 0.4) const;

  // {"samples", "bpm_mean", "bpm_normal_range", "restless_threshold"}
  Json summary() const;

  long samples() const { return n_; }

 private:
  double alpha_;
  std::optional<double> br_mean_;
  double br_var_ = 0.0;
  std::optional<double> move_mean_;
  long n_ = 0;
};

}  // namespace nuni
