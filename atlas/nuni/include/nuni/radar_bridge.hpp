// MR60BHA2(XIAO ESP32-C6, ESPHome) → sensor/radar 브리지 — radar_bridge.py 이식.
//
// 레이더 키트의 ESPHome 펌웨어(deploy/mr60bha2.yaml)가 상태를 MQTT 평문으로 발행하면,
// 이 브리지가 받아 숨결지기 표준 형식(sensor/radar)으로 1초마다 다시 발행한다.
//
// 레이더가 직접 주지 않는 값을 만든다:
// - movement: 최근 창의 거리 변동폭(max-min)으로 움직임(0~1)을 유도
// - apnea   : 원본 호흡값이 APNEA_LOW_BR 이하인 상태가 APNEA_HOLD_S 이상 이어지면 무호흡 의심
//             (2026-11-05 결정: 5회/분 이하 20초. 영아 무호흡 임상 정의·가정용 모니터 기본값이 20초)
// 호흡값 게이팅: 재실·유효 거리·정지 조건을 모두 만족하는 값만 유효로 보고, 최근 창의 중앙값을 발행한다.
// 화면 표시: 방금 값이 게이트에서 떨어져도 창에 유효값이 남아 있으면 그 중앙값을 유지한다(깜빡임 방지).
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
  double apnea_hold_s = 20;             // NUNI_APNEA_HOLD_S — 낮은 호흡이 이만큼 이어지면 무호흡
  double apnea_low_br = 5;              // NUNI_APNEA_LOW_BR — 원본이 이 값 이하이면 '호흡 약함'으로 센다
  double apnea_require_prior_s = 120;   // NUNI_APNEA_PRIOR_S — 이 시간 안에 유효 호흡을 본 적이 있어야 카운트 시작.
                                        //   마지막 유효 호흡 후 이 시간이 지나면 무호흡 판정도 끝낸다(예전 동작, 정지 물체 오경보 방지)
  double stale_s = 10;                  // NUNI_RADAR_STALE_S
  double move_window_s = 2.0;           // NUNI_MOVE_WINDOW_S
  double move_scale_cm = 30;            // NUNI_MOVE_SCALE_CM — 이만큼 변동 = 움직임 1.0
  double br_min = 6;                    // NUNI_BR_MIN — 이 미만은 미검출(5 이하는 무호흡 카운트 쪽에서 다룸)
  double br_max = 60;                   // NUNI_BR_MAX — 영아 상한
  double dist_min_cm = 0;               // NUNI_DIST_MIN_CM — 이 값 '초과'만 유효(대상이 없으면 0.00이 온다)
  double dist_max_cm = 100;             // NUNI_DIST_MAX_CM — 호흡 유효거리
  double move_reject = 0.5;             // NUNI_MOVE_REJECT — 이보다 크게 움직이면 호흡값 신뢰 불가
  double smooth_window_s = 8;           // NUNI_BR_SMOOTH_S — 이 창의 중앙값을 발행(표시 유지 시간이기도 함)
  double tof_leave_mm = 150;            // NUNI_TOF_LEAVE_MM — 카운트 중 난간 ToF가 이만큼 바뀌면 이탈로 보고 멈춤
  double tof_stale_s = 3;               // NUNI_TOF_STALE_S — 이보다 오래된 ToF는 '모름'

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

  // 난간 ToF 측정 하나. 무효면 nullopt(→ 변화 여부를 '모름'으로 둔다). 무호흡 카운트의 이탈 판단에만 쓴다.
  void on_tof(std::optional<double> distance_mm, double now);

  // 현재 수신 상태 → sensor/radar 메시지. 데이터가 없거나 오래됐으면 nullopt.
  std::optional<Json> step(double now);

  const RadarConfig& config() const { return cfg_; }

 private:
  double movement(double now);
  bool in_range(std::optional<double> dist) const;
  bool breath_is_trustworthy(std::optional<double> bpm, std::optional<double> dist, bool presence, double move) const;
  std::optional<double> smoothed_breath(double now);
  void update_low_breath(std::optional<double> raw, bool presence, std::optional<double> dist, bool movement_valid,
                         double move, double now);
  void stop_low_breath();

  RadarConfig cfg_;
  std::optional<double> breath_, heart_, distance_;  // NaN 값도 그대로 보관(Python과 같음)
  std::optional<bool> presence_;
  double last_rx_ = 0.0;
  double breath_rx_ = -1, heart_rx_ = -1, distance_rx_ = -1, presence_rx_ = -1;
  double used_breath_rx_ = -1;
  unsigned long breath_seq_ = 0;
  std::deque<std::pair<double, double>> dist_hist_;  // (ts, distance_cm)
  std::deque<std::pair<double, double>> br_hist_;    // (ts, bpm) — 게이팅을 통과한 유효값만
  std::optional<double> low_since_;                  // 원본이 apnea_low_br 이하로 떨어진 시각(카운트 중일 때만)
  std::optional<double> low_baseline_;               // 카운트를 시작할 때의 마지막 유효 호흡 시각(대상이 사라져도 유지)
  std::optional<double> last_valid_;                 // 마지막 유효 호흡 시각
  std::optional<double> tof_mm_;                     // 최근 유효 ToF 거리
  double tof_rx_ = -1;                               // 최근 ToF 수신 시각(무효 측정 포함)
  std::optional<double> tof_ref_;                    // 카운트를 시작할 때의 ToF 거리
};

// Python float(raw)와 같은 규칙으로 숫자를 읽는다(앞뒤 공백 허용, nan·inf 허용). 실패하면 nullopt.
std::optional<double> parse_py_float(const std::string& raw);

}  // namespace nuni
