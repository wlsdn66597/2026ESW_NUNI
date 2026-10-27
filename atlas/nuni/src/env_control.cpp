#include "nuni/env_control.hpp"

namespace nuni::env_control {

std::vector<Action> recommend(const Json& env, const std::string& sleep_state) {
  if (!truthy(env)) return {};
  std::vector<Action> actions;
  const double co2 = num_or(pyget(env, "co2", 0), 0);
  const double hum = num_or(pyget(env, "humidity", 50), 50);
  const double temp = num_or(pyget(env, "temp", 22), 22);
  const double lux = num_or(pyget(env, "lux", 0), 0);

  if (co2 > CO2_HIGH) actions.emplace_back("ventilate", "CO₂ 높음 → 외기 환기 확인");
  if (hum < HUM_LOW) actions.emplace_back("humidify", "습도 낮음 → 가습 고려");
  else if (hum > HUM_HIGH) actions.emplace_back("dehumidify", "습도 높음 → 제습/환기");
  if (temp > TEMP_HIGH) actions.emplace_back("cool", "온도 높음 → 냉방·통풍");
  else if (temp < TEMP_LOW) actions.emplace_back("heat", "온도 낮음 → 난방");
  if ((sleep_state == "calm_sleep" || sleep_state == "restless") && lux > SLEEP_LUX)
    actions.emplace_back("dim_light", "수면 중 밝음 → 조명 낮춤");
  return actions;
}

}  // namespace nuni::env_control
