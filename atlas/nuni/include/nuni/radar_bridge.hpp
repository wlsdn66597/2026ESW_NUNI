// MR60BHA2(XIAO ESP32-C6, ESPHome) → sensor/radar 브리지 — radar_bridge.py 이식.
//
// 레이더 키트의 ESPHome 펌웨어(deploy/mr60bha2.yaml)가 상태를 MQTT 평문으로 발행하면,
// 이 브리지가 받아 숨결지기 표준 형식(sensor/radar)으로 1초마다 다시 발행한다.
//
// 레이더가 직접 주지 않는 값을 만든다:
// - movement: 최근 창의 거리 변동폭(max-min)으로 움직임(0~1)을 유도
// - apnea   : '잡히던 호흡이 멈춘' 상태가 APNEA_HOLD_S 이상 이어질 때만 무호흡 의심
// 호흡값 게이팅: 재실·유효 거리·정지 조건을 모두 만족하는 값만 유효로 보고, 최근 창의 중앙값을 발행한다.
#pragma once

#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nuni/json_util.hpp"

namespace nuni {

struct RadarConfig {
  std::string prefix = "mr60bha2";      // NUNI_ESPHOME_PREFIX
  double apnea_hold_s = 6;              // NUNI_APNEA_HOLD_S
  double apnea_require_prior_s = 120;   // NUNI_APNEA_PRIOR_S — 이 시간 안에 유효 호흡을 본 적이 있어야 무호흡 판정
  double stale_s = 10;                  // NUNI_RADAR_STALE_S
  double move_window_s = 2.0;           // NUNI_MOVE_WINDOW_S
  double move_scale_cm = 30;            // NUNI_MOVE_SCALE_CM — 이만큼 변동 = 움직임 1.0
  double br_min = 5;                    // NUNI_BR_MIN — 이 미만은 미검출
  double br_max = 60;                   // NUNI_BR_MAX — 영아 상한
  double dist_min_cm = 20;              // NUNI_DIST_MIN_CM
  double dist_max_cm = 150;             // NUNI_DIST_MAX_CM — 호흡 유효거리(모듈 스펙 ~1.5m)
  double move_reject = 0.5;             // NUNI_MOVE_REJECT — 이보다 크게 움직이면 호흡값 신뢰 불가
  double smooth_window_s = 8;           // NUNI_BR_SMOOTH_S — 이 창의 중앙값을 발행

  // 환경변수에서 읽는다(Python과 같은 이름·기본값).
  static RadarConfig from_env();
};

class RadarBridge {
 public:
  explicit RadarBridge(RadarConfig cfg = RadarConfig::from_env());

  // 구독할 ESPHome 토픽들: <prefix>/breath_rate, heart_rate, presence, distance
  std::vector<std::string> esphome_topics() const;

  // ESPHome 상태값(평문) 수신 → 내부 상태 갱신. now = 받은 시각.
  void on_esphome(const std::string& topic, const std::string& raw, double now);

  // 현재 수신 상태 → sensor/radar 메시지. 데이터가 없거나 오래됐으면 nullopt.
  std::optional<Json> step(double now);

  const RadarConfig& config() const { return cfg_; }

 private:
  double movement(double now);
  bool breath_is_trustworthy(std::optional<double> bpm, std::optional<double> dist, bool presence, double move) const;
  std::optional<double> smoothed_breath(double now);
  bool derive_apnea(bool presence, std::optional<double> dist, double move, double now);

  RadarConfig cfg_;
  std::optional<double> breath_, heart_, distance_;  // NaN 값도 그대로 보관(Python과 같음)
  std::optional<bool> presence_;
  double last_rx_ = 0.0;
  double breath_rx_ = -1, heart_rx_ = -1, distance_rx_ = -1, presence_rx_ = -1;
  double used_breath_rx_ = -1;
  unsigned long breath_seq_ = 0;
  std::deque<std::pair<double, double>> dist_hist_;  // (ts, distance_cm)
  std::deque<std::pair<double, double>> br_hist_;    // (ts, bpm) — 게이팅을 통과한 유효값만
  std::optional<double> no_valid_since_;             // 유효 호흡이 끊긴 시각
  std::optional<double> last_valid_;                 // 마지막 유효 호흡 시각
};

// Python float(raw)와 같은 규칙으로 숫자를 읽는다(앞뒤 공백 허용, nan·inf 허용). 실패하면 nullopt.
std::optional<double> parse_py_float(const std::string& raw);

}  // namespace nuni
