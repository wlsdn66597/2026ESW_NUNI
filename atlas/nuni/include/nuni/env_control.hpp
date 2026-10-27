// 선제 환경 제어(권장 동작) — env_control.py 이식.
// 환경 센서(+수면 상태)를 보고 권장 동작을 만든다. 실제 구동(가습기 릴레이 등)은 이 권고를 받는 쪽이 한다.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "nuni/json_util.hpp"

namespace nuni::env_control {

inline constexpr double CO2_HIGH = 1000;
inline constexpr double HUM_LOW = 35;
inline constexpr double HUM_HIGH = 60;
inline constexpr double TEMP_HIGH = 26;
inline constexpr double TEMP_LOW = 18;
inline constexpr double SLEEP_LUX = 40;

using Action = std::pair<std::string, std::string>;  // (action, reason)

// env: 마지막 sensor/env 메시지(없으면 null). sleep_state: sleep_state::classify 결과의 state("" = 없음).
std::vector<Action> recommend(const Json& env, const std::string& sleep_state = "");

}  // namespace nuni::env_control
