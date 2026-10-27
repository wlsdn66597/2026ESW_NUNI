// MQTT 토픽 규격 — topics.py를 그대로 옮긴 것.
// 가상 센서든 실물 센서든, Python이든 C++이든 같은 토픽·같은 JSON 형식으로 주고받는다.
#pragma once

#include "nuni/json_util.hpp"

namespace nuni::topics {
inline constexpr const char* TOF_EVENT = "event/tof";
inline constexpr const char* PI4_STATUS = "status/pi4";

inline constexpr const char* RADAR = "sensor/radar";         // 60GHz 레이더 (호흡·움직임·재실)
inline constexpr const char* ENV = "sensor/env";             // 환경 센서 (온습도·CO2·조도)
inline constexpr const char* TOF = "sensor/tof";             // 난간 ToF 거리 (아기가 일어나면 거리가 줄어든다)
inline constexpr const char* CRY = "audio/cry";              // 울음 감지 결과
inline constexpr const char* VOICE = "audio/voice";          // 부모 음성 인텐트
inline constexpr const char* FUSION_STATE = "fusion/state";  // 융합 판단 현재 상태
inline constexpr const char* ALERT = "fusion/alert";         // 경보 이벤트
inline constexpr const char* CONTROL = "control/action";     // 선제 환경 제어 권고
inline constexpr const char* SOOTHE = "control/soothe";      // 아기 일어남 → 부모 목소리 재생(사건 기록)
inline constexpr const char* VIBRATE = "control/vibrate";    // 진동 직접 요청(시험용, 파이 4 nunivibe)
inline constexpr const char* CRY_EPISODE = "cry/episode";    // 울음 구간 시작·끝·버림(CryLog)
inline constexpr const char* CRY_HINT = "cry/hint";          // 울음 1·2순위 힌트(cry_personal.hint와 같은 키 + id)
inline constexpr const char* CRY_FEEDBACK = "cry/feedback";  // 부모 답·'안 그쳤어요'·수유 기록(대시보드 → 기록)

// breathing_rate: 회/분, movement: 0~1, presence: 재실 여부
inline Json radar_msg(double breathing_rate, double movement, bool presence, double ts) {
  return Json{{"breathing_rate", breathing_rate}, {"movement", movement}, {"presence", presence}, {"ts", ts}};
}

inline Json env_msg(double temp, double humidity, int co2, double lux, double ts) {
  return Json{{"temp", temp}, {"humidity", humidity}, {"co2", co2}, {"lux", lux}, {"ts", ts}};
}

// cls: hungry/sleepy/discomfort/none, confidence: 0~1
inline Json cry_msg(bool is_crying, const std::string& cls, double confidence, double ts) {
  return Json{{"is_crying", is_crying}, {"cls", cls}, {"confidence", confidence}, {"ts", ts}};
}

inline Json voice_msg(const std::string& intent, double ts) { return Json{{"intent", intent}, {"ts", ts}}; }

}  // namespace nuni::topics
