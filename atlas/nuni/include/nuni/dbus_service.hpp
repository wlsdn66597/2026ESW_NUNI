// atlas D-Bus 서비스 — com.atlas.NuniFusion1
//
// atlas 서비스 매니저는 serviceinfo.json의 id와 같은 D-Bus 이름을 이 프로세스가 가져가면 RUNNING으로 본다.
// 기기 안의 앱(Flutter 대시보드 등)은 MQTT 대신 이 인터페이스로 상태를 읽고 신호를 받을 수 있다.
//
//   이름/경로/인터페이스: com.atlas.NuniFusion1  /com/atlas/NuniFusion1  com.atlas.NuniFusion1
//   메서드  Ping() → s               "pong:<가동 초>"
//           GetState() → s           마지막 fusion/state JSON
//           GetSensors() → s         {"radar":…, "cry":…, "env":…} 마지막으로 받은 센서 메시지
//           GetAlerts() → s          최근 경보 [{"level","reason","ts"}, …] (24시간, 최대 200개)
//           GetCryLog() → s          울음 기록 상태(CryLog::snapshot — 지금 울음·힌트·답 없는 울음·최근 울음들)
//           AnswerCry(s id, s action) → s     "무엇으로 그쳤나요?" 답 → {"ok":true,…} 또는 {"ok":false,"error":…}
//           AnswerCryNot(s id, s action) → s  "해 봤는데 안 그쳤어요"
//           LogFeed() → s            지금 수유했다고 기록
//   신호    StateChanged(s)          fusion/state JSON (발행할 때마다)
//           Alert(s level, s reason) 경보 이벤트(fusion/alert)
//           CryDetected(d score)     울음 감지(audio/cry의 is_crying이 거짓→참이 될 때)
//           CryEpisode(s)            울음 기록 사건 {"event": start·hint·end·drop·answer·not·feed, …}
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "nuni/json_util.hpp"

namespace nuni {

class DbusService {
 public:
  virtual ~DbusService() = default;
  virtual void emit_state(const Json& state) = 0;
  virtual void emit_alert(const std::string& level, const std::string& reason) = 0;
  virtual void emit_cry(double score) = 0;
  virtual void emit_cry_event(const Json& event) = 0;
  virtual void set_sensor(const std::string& key, const Json& msg) = 0;  // key: radar / cry / env
};

// 메서드가 부르는 처리 함수(비어 있으면 그 메서드는 {"ok":false} / 빈 값을 돌려준다). D-Bus 스레드에서 불린다.
struct DbusHandlers {
  std::function<std::string()> get_alerts;
  std::function<std::string()> get_cry_log;
  std::function<std::string(const std::string& id, const std::string& action)> answer_cry;
  std::function<std::string(const std::string& id, const std::string& action)> answer_cry_not;
  std::function<std::string()> log_feed;
};

inline constexpr const char* kDbusName = "com.atlas.NuniFusion1";
inline constexpr const char* kDbusPath = "/com/atlas/NuniFusion1";

// system_bus=false면 세션 버스(개발 PC 시험용). 실패하면 nullptr(서비스는 D-Bus 없이 계속 돈다).
std::unique_ptr<DbusService> make_dbus_service(bool system_bus, DbusHandlers handlers = {});

}  // namespace nuni
