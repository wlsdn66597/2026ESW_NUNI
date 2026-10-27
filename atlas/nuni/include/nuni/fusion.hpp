// 멀티모달 융합 판단 엔진 — fusion.py 이식.
//
// decide(): 레이더 + 울음 + 환경을 교차검증하는 규칙(핵심 차별점)
// Fusion  : 최신 신호를 모아 판단하고 디바운스·쿨다운을 거쳐 상태/경보/환경 권고를 발행한다.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "nuni/json_util.hpp"
#include "nuni/personalization.hpp"
#include "nuni/clock.hpp"
#include <map>
#include <set>

namespace nuni {

struct Decision {
  std::string level;  // normal / attention / alert
  std::vector<std::string> reasons;
};

// 판단 규칙(상태 없음). radar·cry·env는 마지막 메시지(없으면 null).
Decision decide(const Json& radar, const Json& cry, const Json& env);

// 비교용: 개별 신호 하나만으로 경보(오탐이 많음).
Decision single_sensor_decide(const Json& radar, const Json& cry, const Json& env);

class Fusion {
 public:
  using PublishFn = std::function<void(const std::string& topic, const Json& payload)>;
  using NowFn = std::function<double()>;

  struct Result {
    std::string level;
    std::vector<std::string> reasons;
    bool alert_emitted = false;
  };

  // publish: 발행 함수(MQTT 등). now: 시계(기본 wall_now). 테스트에서는 가짜 시계를 넣는다.
  explicit Fusion(PublishFn publish, int debounce_n = 2, double cooldown_s = 10, NowFn now = steady_now);
  Result tick();
  Json current_sensors() const;

  // sensor/radar · audio/cry · sensor/env 메시지 하나를 받아 판단·발행한다. 다른 토픽은 무시하고 판단만 다시 한다.
  Result on_message(const std::string& topic, const Json& payload);

  // 한 틱에 세 모달을 한꺼번에 갱신하고 1회 판단(시계열 평가용). null이면 갱신하지 않는다.
  Result update(const Json& radar, const Json& cry, const Json& env);

  // 마지막으로 발행한 fusion/state(아직 없으면 null). D-Bus GetState 등에 쓴다.
  const Json& last_state() const { return last_state_; }

 private:
  std::string debounce(const std::string& level);
  bool cooldown_allows(const std::string& level, const std::vector<std::string>& reasons);
  Result process();

  PublishFn publish_;
  NowFn now_;
  Json radar_, cry_, env_;
  std::string last_ = "normal";
  int debounce_n_;
  double cooldown_s_;
  std::string candidate_ = "normal";
  double candidate_since_ = -1;
  std::map<std::string,double> received_;
  std::map<std::string,std::pair<std::string,long long>> sequences_;
  std::map<std::string,std::set<std::string>> retired_;
  std::string last_personal_id_;
  bool accept(const std::string& topic,const Json& payload);
  Json fresh(const std::string& topic,const Json& payload,double ttl) const;
  bool has_last_alert_ = false;
  std::string last_alert_key_;
  double last_alert_ts_ = 0.0;
  Personalizer personalizer_;
  Json last_state_;
};

}  // namespace nuni
