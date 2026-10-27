// nunivibe — 파이 4 진동 노드. 파이 5 브로커의 경보를 받아 GPIO로 진동 모듈을 켠다.
//
//   fusion/alert    {"level": "alert" | "attention", ...}  → 경보 패턴(길게 3번) / 주의 패턴(짧게 1번)
//   control/vibrate {"pattern": "alert" | "attention" | "test"}  → 손으로 시험할 때
//
// GPIO는 atlas PeripheralManager1(D-Bus)로 다룬다: EnablePeripheral("GPIO_n") → ActiveType=0(ACTIVE_HIGH) → Open(1=OUT)
// → WriteValue(1/0). 기본 ActiveType은 ACTIVE_LOW라 1을 쓰면 핀이 LOW가 되므로 먼저 바꾼다.
// PWM이 없어 세기 조절은 못 하고 켜고 끄는 패턴만 낸다. 진동 모듈은 드라이버(트랜지스터) 모듈을 거쳐 연결한다.
//
// 설정(환경변수): NUNI_MQTT_HOST(기본 10.77.0.5 = 파이 5), NUNI_MQTT_PORT, NUNI_VIBE_GPIO(기본 18),
//                NUNI_VIBE_DRY=1이면 GPIO 없이 로그만.
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "nuni/json_util.hpp"
#include "nuni/mqtt.hpp"
#include "nuni/topics.hpp"
#ifdef NUNI_WITH_DBUS
#include <sdbus-c++/sdbus-c++.h>
#endif

using namespace nuni;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

std::string env_str(const char* k, const std::string& def = "") {
  const char* v = std::getenv(k);
  return v ? std::string(v) : def;
}

// (켜는 ms, 끄는 ms) 묶음
using Pattern = std::vector<std::pair<int, int>>;

Pattern pattern_for(const std::string& name) {
  if (name == "alert") return {{800, 400}, {800, 400}, {800, 0}};
  if (name == "attention") return {{300, 0}};
  if (name == "test") return {{200, 200}, {200, 0}};
  return {};
}

class Gpio {
 public:
  virtual ~Gpio() = default;
  virtual void write(bool on) = 0;
};

class DryGpio : public Gpio {
 public:
  void write(bool on) override { std::cerr << "[vibe] (dry) " << (on ? "ON" : "OFF") << "\n"; }
};

#ifdef NUNI_WITH_DBUS
class PeripheralGpio : public Gpio {
 public:
  explicit PeripheralGpio(const std::string& name) {
    conn_ = sdbus::createSystemBusConnection();
    auto mgr = sdbus::createProxy(*conn_, sdbus::ServiceName(kSvc), sdbus::ObjectPath("/com/atlas/PeripheralManager1"));
    // 부팅 직후에는 Peripherals를 한 번 읽기 전까지 EnablePeripheral이 PeripheralNotFound를 낸다(2026-10-20 Pi 4,
    // 재부팅 뒤 59번 연속 실패 → busctl로 Peripherals를 읽은 다음부터 성공). 그래서 먼저 목록을 읽는다.
    mgr->getProperty("Peripherals").onInterface(kSvc).get<std::vector<sdbus::Struct<std::string, std::string>>>();
    // 이전 실행이 켜 둔 채 끝났으면 PeripheralAlreadyEnabled — 그때는 정해진 경로를 그대로 쓴다
    sdbus::ObjectPath path("/com/atlas/PeripheralManager1/" + name);
    try {
      mgr->callMethod("EnablePeripheral").onInterface(kSvc).withArguments(name).storeResultsTo(path);
    } catch (const sdbus::Error& e) {
      if (e.getName().find("AlreadyEnabled") == std::string::npos) throw;
    }
    pin_ = sdbus::createProxy(*conn_, sdbus::ServiceName(kSvc), path);
    try {  // 이미 ACTIVE_HIGH면 실패할 수 있다(2026-10-19 Pi 4: 값이 0인데 Set이 internal error) → 값만 확인
      pin_->setProperty("ActiveType").onInterface(kGpio).toValue(uint32_t(0));
    } catch (const sdbus::Error&) {
    }
    const uint32_t active = pin_->getProperty("ActiveType").onInterface(kGpio).get<uint32_t>();
    if (active != 0) std::cerr << "[vibe] 경고: ActiveType=" << active << "(ACTIVE_LOW) — 1을 쓰면 핀이 LOW가 됨\n";
    try {
      pin_->callMethod("Open").onInterface(kGpio).withArguments(uint32_t(1));
    } catch (const sdbus::Error&) {  // 열린 채 남아 있으면 닫고 다시 연다
      pin_->callMethod("Close").onInterface(kGpio);
      pin_->callMethod("Open").onInterface(kGpio).withArguments(uint32_t(1));
    }
    write(false);
    std::cerr << "[vibe] " << name << " 출력으로 열림 (" << path << ")\n";
  }
  ~PeripheralGpio() override {
    try {
      write(false);
      pin_->callMethod("Close").onInterface(kGpio);
    } catch (const sdbus::Error&) {
    }
  }
  void write(bool on) override { pin_->callMethod("WriteValue").onInterface(kGpio).withArguments(uint32_t(on ? 1 : 0)); }

 private:
  static constexpr const char* kSvc = "com.atlas.PeripheralManager1";
  static constexpr const char* kGpio = "com.atlas.PeripheralManager1.GPIO";
  std::unique_ptr<sdbus::IConnection> conn_;
  std::unique_ptr<sdbus::IProxy> pin_;
};
#endif

// 패턴은 별도 스레드에서 낸다(MQTT 수신을 막지 않게). 새 요청이 오면 지금 패턴을 끊고 새 것으로 바꾼다.
class Vibrator {
 public:
  explicit Vibrator(std::unique_ptr<Gpio> gpio) : gpio_(std::move(gpio)), th_([this] { loop(); }) {}
  ~Vibrator() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      quit_ = true;
    }
    cv_.notify_all();
    th_.join();
  }
  void play(const std::string& name) {
    Pattern p = pattern_for(name);
    if (p.empty()) return;
    std::cerr << "[vibe] 패턴 " << name << "\n";
    {
      std::lock_guard<std::mutex> lk(mu_);
      pending_ = std::move(p);
      ++gen_;
    }
    cv_.notify_all();
  }

 private:
  void loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!quit_) {
      cv_.wait(lk, [this] { return quit_ || !pending_.empty(); });
      if (quit_) break;
      Pattern p = std::move(pending_);
      pending_.clear();
      const long gen = gen_;
      for (const auto& step : p) {
        if (!set(lk, true) || !hold(lk, step.first, gen)) break;
        if (!set(lk, false) || !hold(lk, step.second, gen)) break;
      }
      set(lk, false);
    }
    set(lk, false);
  }
  bool set(std::unique_lock<std::mutex>& lk, bool on) {
    lk.unlock();
    bool ok = true;
    try {
      gpio_->write(on);
    } catch (const std::exception& e) {
      std::cerr << "[vibe] GPIO 쓰기 실패: " << e.what() << "\n";
      ok = false;
    }
    lk.lock();
    return ok;
  }
  // ms만큼 기다린다. 그 사이 새 요청이 오거나 끝내면 false.
  bool hold(std::unique_lock<std::mutex>& lk, int ms, long gen) {
    if (ms <= 0) return true;
    return !cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return quit_ || gen_ != gen; });
  }

  std::unique_ptr<Gpio> gpio_;
  std::mutex mu_;
  std::condition_variable cv_;
  Pattern pending_;
  long gen_ = 0;
  bool quit_ = false;
  std::thread th_;
};

}  // namespace

int main(int argc, char** argv) {
  std::string host = env_str("NUNI_MQTT_HOST", "10.77.0.5");
  int port = std::atoi(env_str("NUNI_MQTT_PORT", "1883").c_str());
  std::string pin = "GPIO_" + env_str("NUNI_VIBE_GPIO", "18");
  bool dry = env_str("NUNI_VIBE_DRY", "0") == "1";
  bool test_only = false;  // 브로커 없이 시험 패턴 한 번 내고 끝
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--host" && i + 1 < argc) host = argv[++i];
    else if (a == "--gpio" && i + 1 < argc) pin = std::string("GPIO_") + argv[++i];
    else if (a == "--dry") dry = true;
    else if (a == "--test") test_only = true;
    else {
      std::cout << "사용법: " << argv[0] << " [--host H] [--gpio N] [--dry] [--test]\n"
                << "  환경변수 NUNI_MQTT_HOST(기본 10.77.0.5), NUNI_MQTT_PORT, NUNI_VIBE_GPIO(기본 18), NUNI_VIBE_DRY=1\n";
      return a == "--help" || a == "-h" ? 0 : 2;
    }
  }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  std::unique_ptr<Gpio> gpio;
#ifdef NUNI_WITH_DBUS
  // 주변장치 관리 서비스가 늦게 준비될 수 있어, 끝내지 않고 3초마다 다시 연다(실패 로그는 처음과 10번마다)
  for (int attempt = 1; !dry && !gpio && !g_stop; ++attempt) {
    try {
      gpio = std::make_unique<PeripheralGpio>(pin);
    } catch (const sdbus::Error& e) {
      if (attempt == 1 || attempt % 10 == 0)
        std::cerr << "[vibe] GPIO 열기 실패(" << attempt << "번째): " << e.getName() << " — " << e.getMessage()
                  << " — 3초 뒤 다시 시도\n";
      std::this_thread::sleep_for(std::chrono::seconds(3));
    }
  }
  if (g_stop) return 0;
#else
  if (!dry) std::cerr << "[vibe] sdbus-c++ 없이 빌드됨 — 로그만 남깁니다\n";
#endif
  if (!gpio) gpio = std::make_unique<DryGpio>();
  Vibrator vib(std::move(gpio));

  if (test_only) {
    vib.play("test");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    return 0;
  }

  MqttClient mqtt(host, port, "nunivibe-" + std::to_string(getpid()));
  mqtt.subscribe(topics::ALERT);
  mqtt.subscribe(topics::VIBRATE);
  mqtt.set_on_connect([&] { std::cerr << "[vibe] 브로커 접속 " << host << ":" << port << "\n"; });
  mqtt.set_on_message([&](const std::string& topic, const std::string& raw) {
    if(mqtt.message_retained()) return;
    auto j = parse_payload(raw);
    if (!j || !j->is_object()) return;
    if (topic == topics::ALERT) vib.play(j->value("level", ""));
    else if (topic == topics::VIBRATE) vib.play(j->value("pattern", ""));
  });
  std::cerr << "[vibe] 시작: " << pin << (dry ? " (dry)" : "") << ", MQTT " << host << ":" << port << "\n";
  while (!g_stop) mqtt.poll(200);
  std::cerr << "[vibe] 종료\n";
  return 0;
}
