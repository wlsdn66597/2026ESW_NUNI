#include "nuni/sleep_state.hpp"

#include <cstdlib>

namespace nuni::sleep_state {

double cry_confirm_threshold() {
  static const double v = [] {
    const char* e = std::getenv("NUNI_FUSION_CRY_THRESHOLD");
    return e ? std::strtod(e, nullptr) : 0.7;
  }();
  return v;
}

std::string ko_label(const std::string& s) {
  if (s == "calm_sleep") return "낮은 움직임";
  if (s == "restless") return "뒤척임";
  if (s == "awake") return "각성";
  if (s == "unknown") return "관측 불가";
  if (s == "hold") return "판정 보류";
  return s;
}

Result classify(const Json& radar, const Json& cry) {
  if (!truthy(radar) || !truthy(pyget(radar,"valid",true)) || !truthy(pyget(radar, "presence"))) return {"unknown", "레이더에서 대상을 확인하지 못함"};

  // breathing_rate가 없으면 breathing_bpm, 둘 다 없으면 None
  const Json br = pyget(radar, "breathing_rate", pyget(radar, "breathing_bpm"));
  // 실물 레이더가 호흡을 못 재는 구간(breath_valid=False)의 0은 '무호흡'이 아니라 '미측정'이다.
  const bool br_valid = truthy(pyget(radar, "breath_valid", false));
  const auto br_num = as_number(br);  // isinstance(breathing_rate, (int, float))
  if (truthy(pyget(radar, "apnea")) || (br_valid && br_num && *br_num < 8))
    return {"hold", "호흡 이상 감지로 수면 판정 보류"};
  if (!br_valid) return {"hold", "호흡 신호 미확보로 판정 보류"};

  const bool crying = truthy(cry) && truthy(pyget(cry, "is_crying")) &&
                      num_or(pyget(cry, "confidence", 1.0), 1.0) >= cry_confirm_threshold();
  const double movement = num_or(pyget(radar, "movement", 0.0), 0.0);

  if (crying || movement >= MOVE_AWAKE) return {"awake", "울음 또는 큰 움직임"};
  if (movement >= MOVE_RESTLESS) return {"restless", "움직임 증가"};
  if (!truthy(pyget(radar,"movement_valid",false))) return {"hold","움직임 신호 미확보"};
  return {"calm_sleep", "호흡 신호 측정·낮은 움직임(수면 여부 미확인)"};
}

}  // namespace nuni::sleep_state
