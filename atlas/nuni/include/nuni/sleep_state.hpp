// 수면/각성 상태 분류 — sleep_state.py 이식.
//
// state: calm_sleep(안정 수면) / restless(뒤척임) / awake(각성)
//        / unknown(재실 없음) / hold(판정 보류 — 사람은 있으나 호흡 판단 불가)
#pragma once

#include <string>

#include "nuni/json_util.hpp"

namespace nuni::sleep_state {

inline constexpr double MOVE_RESTLESS = 0.4;  // 이 이상이면 뒤척임
inline constexpr double MOVE_AWAKE = 0.7;     // 이 이상이면 각성

// 환경변수 NUNI_FUSION_CRY_THRESHOLD (기본 0.7). 처음 부를 때 한 번 읽는다(Python 모듈 로드 시점과 같음).
double cry_confirm_threshold();

// 한국어 표시 이름(KO_LABEL).
std::string ko_label(const std::string& state);

struct Result {
  std::string state;
  std::string reason;
};

// radar·cry는 마지막으로 받은 메시지(없으면 null).
Result classify(const Json& radar, const Json& cry);

}  // namespace nuni::sleep_state
