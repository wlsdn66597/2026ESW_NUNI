// com.atlas.NuniFusion1 D-Bus 서비스 — sdbus-c++ 2.x (atlas SDK에 포함, LG 샘플 lifecycleservice와 같은 방식).
#include "nuni/dbus_service.hpp"

#include <sdbus-c++/sdbus-c++.h>

#include <chrono>
#include <iostream>
#include <mutex>

namespace nuni {

namespace {

constexpr const char* kIface = "com.atlas.NuniFusion1";

class SdbusService : public DbusService {
 public:
  SdbusService(bool system_bus, DbusHandlers h) : h_(std::move(h)), start_(std::chrono::steady_clock::now()) {
    // 서비스 매니저는 이 이름(= serviceinfo.json의 id)이 버스에 나타나면 RUNNING으로 본다
    conn_ = system_bus ? sdbus::createSystemBusConnection(sdbus::ServiceName(kDbusName))
                       : sdbus::createSessionBusConnection(sdbus::ServiceName(kDbusName));
    obj_ = sdbus::createObject(*conn_, sdbus::ObjectPath(kDbusPath));
    obj_->addVTable(
            sdbus::registerMethod("Ping").withOutputParamNames("pong").implementedAs([this]() { return ping(); }),
            sdbus::registerMethod("GetState").withOutputParamNames("state_json").implementedAs([this]() {
              std::lock_guard<std::mutex> lk(mu_);
              return state_.is_null() ? std::string("{}") : dump(state_);
            }),
            sdbus::registerMethod("GetSensors").withOutputParamNames("sensors_json").implementedAs([this]() {
              std::lock_guard<std::mutex> lk(mu_);
              return dump(sensors_);
            }),
            sdbus::registerMethod("GetAlerts").withOutputParamNames("alerts_json").implementedAs([this]() {
              return h_.get_alerts ? h_.get_alerts() : std::string("[]");
            }),
            sdbus::registerMethod("GetCryLog").withOutputParamNames("log_json").implementedAs([this]() {
              return h_.get_cry_log ? h_.get_cry_log() : std::string("{}");
            }),
            sdbus::registerMethod("AnswerCry")
                .withInputParamNames("id", "action")
                .withOutputParamNames("result_json")
                .implementedAs([this](const std::string& id, const std::string& action) {
                  return h_.answer_cry ? h_.answer_cry(id, action) : unavailable();
                }),
            sdbus::registerMethod("AnswerCryNot")
                .withInputParamNames("id", "action")
                .withOutputParamNames("result_json")
                .implementedAs([this](const std::string& id, const std::string& action) {
                  return h_.answer_cry_not ? h_.answer_cry_not(id, action) : unavailable();
                }),
            sdbus::registerMethod("LogFeed").withOutputParamNames("result_json").implementedAs([this]() {
              return h_.log_feed ? h_.log_feed() : unavailable();
            }),
            sdbus::registerSignal("StateChanged").withParameters<std::string>("state_json"),
            sdbus::registerSignal("Alert").withParameters<std::string, std::string>("level", "reason"),
            sdbus::registerSignal("CryDetected").withParameters<double>("score"),
            sdbus::registerSignal("CryEpisode").withParameters<std::string>("event_json"))
        .forInterface(kIface);
    conn_->enterEventLoopAsync();
    std::cerr << "[dbus] " << kDbusName << " 등록 (" << (system_bus ? "system" : "session") << " bus)\n";
  }

  ~SdbusService() override {
    conn_->leaveEventLoop();
    obj_.reset();
  }

  void emit_state(const Json& state) override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      state_ = state;
    }
    obj_->emitSignal("StateChanged").onInterface(kIface).withArguments(dump(state));
  }

  void emit_alert(const std::string& level, const std::string& reason) override {
    obj_->emitSignal("Alert").onInterface(kIface).withArguments(level, reason);
  }

  void emit_cry(double score) override { obj_->emitSignal("CryDetected").onInterface(kIface).withArguments(score); }

  void emit_cry_event(const Json& event) override {
    obj_->emitSignal("CryEpisode").onInterface(kIface).withArguments(dump(event));
  }

  void set_sensor(const std::string& key, const Json& msg) override {
    std::lock_guard<std::mutex> lk(mu_);
    sensors_[key] = msg;
  }

 private:
  static std::string unavailable() { return dump(Json{{"ok", false}, {"error", "울음 기록이 꺼져 있어요"}}); }

  std::string ping() {
    const auto up = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_);
    return "pong:" + std::to_string(up.count());
  }

  DbusHandlers h_;
  std::unique_ptr<sdbus::IConnection> conn_;
  std::unique_ptr<sdbus::IObject> obj_;
  std::chrono::steady_clock::time_point start_;
  std::mutex mu_;
  Json state_;
  Json sensors_ = Json::object();
};

}  // namespace

std::unique_ptr<DbusService> make_dbus_service(bool system_bus, DbusHandlers handlers) {
  try {
    return std::make_unique<SdbusService>(system_bus, std::move(handlers));
  } catch (const sdbus::Error& e) {
    std::cerr << "[dbus] 등록 실패: " << e.getName() << " — " << e.getMessage() << " (D-Bus 없이 계속)\n";
  } catch (const std::exception& e) {
    std::cerr << "[dbus] 등록 실패: " << e.what() << " (D-Bus 없이 계속)\n";
  }
  return nullptr;
}

}  // namespace nuni
