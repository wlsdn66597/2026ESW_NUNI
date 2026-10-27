// nunifusion — 숨결지기 atlas 네이티브 서비스(com.atlas.NuniFusion1)
//
// 한 프로세스에서 세 모듈을 돌린다(각각 끌 수 있음). 모듈끼리는 Python 분산 모드와 똑같이 MQTT 브로커를 거친다.
//   radar  : ESPHome(mr60bha2/*) 구독 → sensor/radar 1초마다 발행      (radar_bridge.py)
//   cry    : 마이크(PulseAudio) → YAMNet(ECO.AI, 없으면 TFLite) → audio/cry 1초마다 발행  (cry_classifier.py 실물 경로)
//   fusion : sensor/radar · audio/cry · sensor/env 구독 → fusion/state · fusion/alert · control/action 발행
//            (fusion.py + sleep_state.py + env_control.py + personalization.py)
//   soothe : Pi 4 event/tof를 검증하고 음성 재생 상태를 control/soothe로 회신
//   crylog : 울음 구간을 나누고 부모 답("무엇으로 그쳤나요?")을 쌓아 1·2순위 힌트를 낸다(cry_personal.py 흐름).
//            cry/episode · cry/hint · cry/feedback 발행, 기록은 /data/nuni/cry
//   + D-Bus: com.atlas.NuniFusion1 이름을 가져가 서비스 매니저에 RUNNING으로 보이고, 상태를 메서드·신호로 내보낸다.
//            대시보드는 여기서 울음 기록을 읽고 답을 보낸다(AnswerCry 등).
//
// 설정은 환경변수(systemd 유닛의 Environment=)로 하고, 시험할 때는 명령줄 옵션으로 덮어쓴다. --help 참고.
#include <signal.h>
#include <sys/wait.h>
#include <spawn.h>
extern char **environ;
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nuni/audio_source.hpp"
#include "nuni/cry_detector.hpp"
#include "nuni/cry_log.hpp"
#include "nuni/fusion.hpp"
#include "nuni/json_util.hpp"
#include "nuni/mqtt.hpp"
#include "nuni/radar_bridge.hpp"
#include "nuni/sensor_hub.hpp"
#include "nuni/clock.hpp"
#include "nuni/topics.hpp"
#ifdef NUNI_WITH_TFLITE
#include "nuni/yamnet_tflite.hpp"
#endif
#ifdef NUNI_WITH_ECOAI
#include "nuni/yamnet_ecoai.hpp"
#endif
#include "nuni/dbus_service.hpp"

using namespace nuni;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

std::string env_str(const char* k, const std::string& def = "") {
  const char* v = std::getenv(k);
  return v ? std::string(v) : def;
}
bool env_on(const char* k, bool def) {
  const char* v = std::getenv(k);
  if (!v) return def;
  return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0 || std::strcmp(v, "off") == 0);
}
double env_num(const char* k, double def) {
  const char* v = std::getenv(k);
  return v ? std::strtod(v, nullptr) : def;
}

std::string exe_dir() {
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  buf[n] = 0;
  std::string p(buf);
  const auto s = p.rfind('/');
  return s == std::string::npos ? "." : p.substr(0, s);
}

bool file_exists(const std::string& p) { return access(p.c_str(), R_OK) == 0; }

std::string default_model() {
  const std::string emb = exe_dir() + "/yamnet_emb.tflite";
  return file_exists(emb) ? emb : exe_dir() + "/yamnet.tflite";
}

struct Options {
  std::string host = env_str("NUNI_MQTT_HOST", "localhost");
  int port = static_cast<int>(env_num("NUNI_MQTT_PORT", 1883));
  std::string user = env_str("NUNI_MQTT_USER"), pass = env_str("NUNI_MQTT_PASS");
  bool radar = env_on("NUNI_ENABLE_RADAR", false);
  bool cry = env_on("NUNI_ENABLE_CRY", true);
  bool fusion = env_on("NUNI_ENABLE_FUSION", true);
  bool dbus = env_on("NUNI_ENABLE_DBUS", true);
  bool dbus_session = env_str("NUNI_DBUS_BUS", "system") == "session";
  // 기본은 실행 파일 옆 yamnet_emb.tflite(실수형 — atlas에서 노트북과 출력이 같음을 확인), 없으면 yamnet.tflite
  std::string model = env_str("NUNI_YAMNET_MODEL", default_model());
  std::string classmap = env_str("NUNI_YAMNET_CLASSMAP", exe_dir() + "/yamnet_class_map.csv");
  int threads = static_cast<int>(env_num("NUNI_YAMNET_THREADS", 1));
  // auto: ECO.AI를 먼저 쓰고, 플러그인이 없으면(Pi 4) TFLite 직접. ecoai / tflite로 하나만 쓰게 할 수 있다
  std::string engine = env_str("NUNI_YAMNET_ENGINE", "auto");
  std::string ecoai_backend = env_str("NUNI_ECOAI_BACKEND", "cpu");  // Pi 5에서 gpu는 cpu보다 느렸다(17.5ms vs 8.8ms)
  double cry_threshold = env_num("NUNI_CRY_THRESHOLD", 0.3);  // cry_classifier.py CRY_THRESHOLD
  int mic_sr = static_cast<int>(env_num("NUNI_MIC_SR", 48000));  // USB 마이크 고유 레이트(cry_classifier.py)
  std::string pulse_server = env_str("NUNI_PULSE_SERVER"), pulse_source = env_str("NUNI_PULSE_SOURCE");
  std::string wav = env_str("NUNI_CRY_WAV");  // 지정하면 마이크 대신 이 파일을 반복 재생
  // 울음 기록(부모 답 쌓기 + 힌트). 기록 폴더는 패키지를 다시 깔아도 남는 /data 아래에 둔다
  bool crylog = env_on("NUNI_ENABLE_CRYLOG", true);
  std::string cry_dir = env_str("NUNI_CRY_DIR", "/data/nuni/cry");
  std::string cry_ctx = env_str("NUNI_CRY_CTX", exe_dir() + "/cry_personal_ctx.json");
  bool cry_record = env_on("NUNI_CRY_RECORD", true);  // 울음 소리·임베딩 저장(기기 안에만)
  double cry_end_gap = env_num("NUNI_CRY_END_GAP", 8);
  int cry_min_windows = static_cast<int>(env_num("NUNI_CRY_MIN_WINDOWS", 3));
  // 아기 일어남 → 부모 목소리. 파일은 패키지를 다시 깔아도 남는 /data 아래에 둔다
  bool soothe = env_on("NUNI_ENABLE_SOOTHE", true);
  std::string soothe_wav = env_str("NUNI_SOOTHE_WAV", "/data/nuni/voice/parent.wav");
  std::string soothe_sink = env_str("NUNI_SOOTHE_SINK");  // 비우면 PulseAudio 기본 출력
  int soothe_volume = static_cast<int>(env_num("NUNI_SOOTHE_VOLUME", 40));  // 0~100%. 아기 자리 50dBA 이하가 되게 실측해 맞춘다
  double soothe_cooldown = env_num("NUNI_SOOTHE_COOLDOWN", 180);
};

void usage(const char* argv0) {
  std::cout << "사용법: " << argv0 << " [옵션]\n"
            << "  --host H / --port P     MQTT 브로커 (NUNI_MQTT_HOST, NUNI_MQTT_PORT; 기본 localhost:1883)\n"
            << "  --no-radar --no-cry --no-fusion --no-dbus   모듈 끄기 (NUNI_ENABLE_*=0)\n"
            << "  --session-bus           D-Bus 세션 버스 사용(개발 PC 시험용, NUNI_DBUS_BUS=session)\n"
            << "  --model PATH            YAMNet .tflite (NUNI_YAMNET_MODEL; 기본 실행 파일 옆 yamnet_emb.tflite → yamnet.tflite)\n"
            << "  --classmap PATH         yamnet_class_map.csv (NUNI_YAMNET_CLASSMAP; 없으면 클래스 19·20)\n"
            << "  --wav PATH              마이크 대신 WAV 반복 재생 (NUNI_CRY_WAV)\n"
            << "  --mic-sr N              마이크 샘플레이트 (NUNI_MIC_SR; 기본 48000)\n"
            << "  --engine E              추론 엔진 auto|ecoai|tflite (NUNI_YAMNET_ENGINE; 기본 auto = ECO.AI → TFLite)\n"
            << "그 밖의 환경변수: NUNI_ECOAI_BACKEND(cpu|gpu), NUNI_CRY_THRESHOLD, NUNI_YAMNET_THREADS,\n"
            << "  NUNI_PULSE_SERVER, NUNI_PULSE_SOURCE,\n"
            << "  울음 기록: NUNI_ENABLE_CRYLOG(1), NUNI_CRY_DIR(/data/nuni/cry), NUNI_CRY_CTX, NUNI_CRY_RECORD(1),\n"
            << "  NUNI_CRY_END_GAP(8초), NUNI_CRY_MIN_WINDOWS(3),\n"
            << "  NUNI_FUSION_CRY_THRESHOLD, 레이더 게이팅 값(NUNI_BR_MIN 등, radar_bridge.py와 같음)\n"
            << "  아기 일어남 → 목소리: NUNI_ENABLE_SOOTHE(1), NUNI_SOOTHE_WAV(/data/nuni/voice/parent.wav),\n"
            << "  NUNI_SOOTHE_SINK(기본 출력), NUNI_SOOTHE_VOLUME(40%), NUNI_SOOTHE_COOLDOWN(180초),\n"
            << "  NUNI_TOF_DROP_MM(150), NUNI_TOF_HOLD_N(3)\n";
}

bool parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string& dst) {
      if (i + 1 >= argc) return false;
      dst = argv[++i];
      return true;
    };
    std::string v;
    if (a == "--help" || a == "-h") { usage(argv[0]); std::exit(0); }
    else if (a == "--no-radar") o.radar = false;
    else if (a == "--no-cry") o.cry = false;
    else if (a == "--no-fusion") o.fusion = false;
    else if (a == "--no-dbus") o.dbus = false;
    else if (a == "--session-bus") o.dbus_session = true;
    else if (a == "--host" && next(v)) o.host = v;
    else if (a == "--port" && next(v)) o.port = std::atoi(v.c_str());
    else if (a == "--model" && next(v)) o.model = v;
    else if (a == "--classmap" && next(v)) o.classmap = v;
    else if (a == "--wav" && next(v)) o.wav = v;
    else if (a == "--mic-sr" && next(v)) o.mic_sr = std::atoi(v.c_str());
    else if (a == "--engine" && next(v)) o.engine = v;
    else {
      std::cerr << "알 수 없는 옵션: " << a << "\n";
      usage(argv[0]);
      return false;
    }
  }
  return true;
}

// 부모 목소리 재생: paplay를 자식 프로세스로 띄운다. 재생 중이면 겹쳐 틀지 않는다.
class VoicePlayer {
 public:
  VoicePlayer(std::string pulse_server, std::string sink, int volume_pct)
      : server_(std::move(pulse_server)), sink_(std::move(sink)), vol_(std::max(0, std::min(100, volume_pct))) {}
  ~VoicePlayer() {
    if (busy()) { kill(pid_, SIGTERM); waitpid(pid_,nullptr,0); pid_=-1; }
  }

  bool busy() {
    reap();
    return pid_ > 0;
  }

  // 시작했으면 true. 파일이 없거나 재생 중이면 false와 이유.
  bool play(const std::string& wav, std::string* why) {
    if (!file_exists(wav)) return *why = "목소리 파일 없음: " + wav, false;
    if (busy()) return *why = "이미 재생 중", false;
    std::vector<std::string> args{"paplay", "--volume=" + std::to_string(65536 * vol_ / 100)};
    if (!sink_.empty()) args.insert(args.end(), {"-d", sink_});
    args.push_back(wav);
    std::vector<char*> argv;
    for(auto& a:args) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<std::string> env_strings;
    for(char** e=environ;*e;++e) {
      if(server_.empty() || std::strncmp(*e,"PULSE_SERVER=",13)!=0) env_strings.emplace_back(*e);
    }
    if(!server_.empty()) env_strings.push_back("PULSE_SERVER="+server_);
    std::vector<char*> child_env;
    for(auto& e:env_strings)child_env.push_back(e.data());
    child_env.push_back(nullptr);
    pid_t pid=-1;
    const int error=posix_spawnp(&pid,"paplay",nullptr,nullptr,argv.data(),child_env.data());
    if(error) return *why=std::strerror(error),false;
    pid_ = pid;
    return true;
  }

  std::optional<int> completion() { reap(); auto result=completed_; completed_.reset(); return result; }
 private:
  void reap() {
    int status=0;
    if(pid_>0 && waitpid(pid_,&status,WNOHANG)==pid_) {
      completed_=WIFEXITED(status)?WEXITSTATUS(status):128;
      pid_=-1;
    }
  }
  std::optional<int> completed_;
  std::string server_, sink_;
  int vol_;
  pid_t pid_ = -1;
};

using CryEventFn = std::function<void(const CryLog::Event&)>;

// 울음 감지 스레드: 1초마다 최근 1.5초로 판정해 audio/cry를 발행한다.
// 울음 기록이 켜져 있으면 그 1초의 점수·임베딩·소리(지난 1초, 16kHz)를 넘겨 울음 구간을 나눈다.
void cry_loop(const Options& o, MqttClient& mqtt, CryLog* cry_log, const CryEventFn& on_cry_event) {
#if !defined(NUNI_WITH_TFLITE) && !defined(NUNI_WITH_ECOAI)
  (void)o;
  (void)mqtt;
  (void)cry_log;
  (void)on_cry_event;
  std::cerr << "[cry] ECO.AI·TFLite 없이 빌드됨 — 울음 감지 끔\n";
#else
  std::unique_ptr<AudioModel> model;
#ifdef NUNI_WITH_ECOAI
  if (o.engine != "tflite") {
    model = make_yamnet_ecoai(o.model, o.threads, o.ecoai_backend);
    if (!model && o.engine == "auto") std::cerr << "[cry] ECO.AI를 쓸 수 없어 TFLite 직접으로 넘어갑니다\n";
  }
#endif
#ifdef NUNI_WITH_TFLITE
  if (!model && o.engine != "ecoai") model = make_yamnet_tflite(o.model, o.threads);
#endif
  if (!model) {
    std::cerr << "[cry] YAMNet 모델을 쓸 수 없어 울음 감지를 끕니다 (" << o.model << ")\n";
    return;
  }
  CryDetector det(std::move(model), file_exists(o.classmap) ? o.classmap : "");
  RingBuffer ring(o.mic_sr, 3.0);
  std::unique_ptr<AudioSource> src = o.wav.empty() ? make_pulse_source(ring, o.pulse_server, o.pulse_source)
                                                   : make_wav_source(ring, o.wav, true);
  bool started = src->start();
  auto last_try = std::chrono::steady_clock::now();
  auto next = std::chrono::steady_clock::now();
  long n = 0;
  bool warned = false;
  while (!g_stop) {
    next += std::chrono::seconds(1);
    std::this_thread::sleep_until(next);
    if (!started || !src->ok()) {  // 마이크가 빠지거나 오디오 서버가 재시작되면 5초마다 다시 연다
      if (!warned) {
        std::cerr << "[cry] 소리 입력 없음(" << src->name() << ") — 5초마다 다시 시도\n";
        warned = true;
      }
      if (std::chrono::steady_clock::now() - last_try > std::chrono::seconds(5)) {
        last_try = std::chrono::steady_clock::now();
        started = src->start();
        if (started) warned = false;
      }
      continue;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const double ts = wall_now();
    CryResult detail;
    auto msg = det.classify(ring.last(1.5), o.mic_sr, o.cry_threshold, ts, &detail);
    if (!msg) continue;
    (*msg)["boot_id"]="audio-"+std::to_string(getpid());
    (*msg)["seq"]=n+1;
    mqtt.publish(topics::CRY, dump(*msg));
    if (cry_log) {
      const auto sec = resample_linear(ring.last(1.0), o.mic_sr);  // 겹치지 않는 지난 1초(녹음용)
      for (const auto& ev : cry_log->tick(ts, detail.cry_score, detail.embedding, sec)) on_cry_event(ev);
    }
    if (++n == 1 || n % 600 == 0) {
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      std::cerr << "[cry] " << n << "번째 판정 " << ms << "ms, 점수 " << (*msg)["confidence"] << "\n";
    }
  }
  src->stop();
#endif
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!parse_args(argc, argv, o)) return 2;
  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  std::cerr << "[nuni] 시작: radar " << o.radar << ", cry " << o.cry << ", fusion " << o.fusion << ", MQTT " << o.host
            << ":" << o.port << "\n";

  MqttClient mqtt(o.host, o.port, "nunifusion-" + std::to_string(getpid()));
  if (!o.user.empty()) mqtt.set_credentials(o.user, o.pass);

  // 울음 기록(부모 답 쌓기 + 1·2순위 힌트)
  std::unique_ptr<CryLog> cry_log;
  if (o.cry && o.crylog) {
    CryLogConfig cc;
    cc.dir = o.cry_dir;
    cc.ctx_path = o.cry_ctx;
    cc.record = o.cry_record;
    cc.min_score = o.cry_threshold;
    cc.end_gap_s = o.cry_end_gap;
    cc.min_windows = o.cry_min_windows;
    cry_log = std::make_unique<CryLog>(cc);
  }

  // 최근 경보(대시보드의 '오늘 밤' 줄에 쓴다)
  std::mutex alerts_mu;
  std::deque<Json> alerts;

  DbusService* dbus_ptr = nullptr;  // 아래에서 만든 뒤 채운다(울음 사건 신호용)
  const CryEventFn on_cry_event = [&](const CryLog::Event& ev) {
    const char* topic = topics::CRY_EPISODE;  // start · end · drop
    if (ev.kind == "hint") topic = topics::CRY_HINT;
    else if (ev.kind == "answer" || ev.kind == "not" || ev.kind == "feed") topic = topics::CRY_FEEDBACK;
    mqtt.publish(topic, dump(ev.data));
    if (dbus_ptr) dbus_ptr->emit_cry_event(ev.data);
    if (ev.kind == "start" || ev.kind == "end" || ev.kind == "answer")
      std::cerr << "[crylog] " << ev.kind << " " << ev.data.value("id", "") << "\n";
  };

#ifdef NUNI_WITH_DBUS
  std::unique_ptr<DbusService> dbus;
  if (o.dbus) {
    DbusHandlers h;
    h.get_alerts = [&] {
      std::lock_guard<std::mutex> lk(alerts_mu);
      Json a = Json::array();
      for (const auto& x : alerts) a.push_back(x);
      return dump(a);
    };
    if (cry_log) {
      auto result = [&](const std::optional<CryLog::Event>& ev, const std::string& err) {
        if (!ev) return dump(Json{{"ok", false}, {"error", err}});
        on_cry_event(*ev);
        return dump(Json{{"ok", true}, {"event", ev->data}});
      };
      h.get_cry_log = [&] { return dump(cry_log->snapshot(wall_now())); };
      h.answer_cry = [&, result](const std::string& id, const std::string& action) {
        std::string err;
        auto ev = cry_log->feedback(id, action, wall_now(), &err);
        return result(ev, err);
      };
      h.answer_cry_not = [&, result](const std::string& id, const std::string& action) {
        std::string err;
        auto ev = cry_log->feedback_not(id, action, wall_now(), &err);
        return result(ev, err);
      };
      h.log_feed = [&, result] { return result(cry_log->log_feed(wall_now()), ""); };
    }
    dbus = make_dbus_service(!o.dbus_session, std::move(h));
    dbus_ptr = dbus.get();
  }
#endif

  long n_radar_out = 0, n_state = 0, n_in = 0;
  bool last_crying = false;

  // fusion이 발행하는 메시지 → MQTT(+ D-Bus 신호)
  Fusion fusion([&](const std::string& topic, const Json& payload) {
    mqtt.publish(topic, dump(payload));
    if (topic == topics::ALERT) {
      std::lock_guard<std::mutex> lk(alerts_mu);
      alerts.push_back(payload);
      const double cutoff = wall_now() - 24 * 3600;
      while (!alerts.empty() && (alerts.size() > 200 || num_or(pyget(alerts.front(), "ts", 0), 0) < cutoff))
        alerts.pop_front();
    }
#ifdef NUNI_WITH_DBUS
    if (dbus) {
      if (topic == topics::FUSION_STATE) dbus->emit_state(payload);
      else if (topic == topics::ALERT)
        dbus->emit_alert(payload.value("level", ""), payload.value("reason", ""));
    }
#endif
    if (topic == topics::FUSION_STATE) ++n_state;
  });

  RadarBridge radar;
  if (o.radar)
    for (const auto& t : radar.esphome_topics()) mqtt.subscribe(t);
  if (o.fusion)
    for (const char* t : {topics::RADAR, topics::CRY, topics::ENV}) mqtt.subscribe(t);

  // Pi 4 has already processed ToF. Pi 5 receives only fresh entry events.
  SootheGate soothe_gate;
  VoicePlayer voice(o.pulse_server, o.soothe_sink, o.soothe_volume);
  std::optional<double> last_play;
  Json playing_event;
  if (o.soothe) {
    mqtt.subscribe(topics::TOF_EVENT);
    mqtt.subscribe(topics::PI4_STATUS);
  }

  bool warned_bad = false;
  mqtt.set_on_message([&](const std::string& topic, const std::string& raw) {
    ++n_in;
    if (o.radar && topic.rfind(radar.config().prefix + "/", 0) == 0) radar.on_esphome(topic, raw, steady_now());
    if (mqtt.message_retained()) return; // snapshots/commands cannot become new observations
    if (o.soothe && topic == topics::PI4_STATUS) {
      if (auto j = parse_payload(raw)) soothe_gate.heartbeat(*j, steady_now());
      return;
    }
    if (o.soothe && topic == topics::TOF_EVENT) {
      auto j=parse_payload(raw);
      if (!j || !soothe_gate.accept(*j,steady_now())) return;
      std::string why;
      const bool cooldown = last_play && steady_now()-*last_play < o.soothe_cooldown;
      const bool started = !cooldown && voice.play(o.soothe_wav,&why);
      if(cooldown) why="cooldown";
      if(started) { last_play=steady_now(); playing_event=*j; }
      mqtt.publish(topics::SOOTHE,dump(Json{{"id",(*j)["id"]},{"status",started?"started":"rejected"},
        {"reason","rail_intrusion"},{"started",started},{"why",why},{"ts",wall_now()}}));
      return;
    }
    if (!o.fusion || !(topic == topics::RADAR || topic == topics::CRY || topic == topics::ENV)) return;
    auto j = parse_payload(raw);
    if (!j || !j->is_object()) {
      // Python 원본은 여기서 예외로 멈춘다. C++은 그 메시지만 버리고 계속 간다.
      if (!warned_bad) std::cerr << "[fusion] JSON 객체가 아닌 메시지를 무시함: " << topic << " " << raw.substr(0, 80) << "\n";
      warned_bad = true;
      return;
    }
    if(pyget(*j,"source")=="pi4-usb") { (*j)["source_ts"]=pyget(*j,"ts"); (*j)["ts"]=wall_now(); }
    fusion.on_message(topic,*j);
#ifdef NUNI_WITH_DBUS
    if(dbus) {
      const auto sensors=fusion.current_sensors();
      for(const char* key:{"radar","cry","env"}) dbus->set_sensor(key,sensors[key]);
      if(topic==topics::CRY) {
        const bool crying=truthy(pyget(sensors["cry"],"is_crying"));
        if(crying && !last_crying)dbus->emit_cry(num_or(pyget(*j,"confidence"),0));
        last_crying=crying;
      }
    }
#endif
    (void)last_crying;

  });

  std::thread cry_th;
  if (o.cry) cry_th = std::thread([&] { cry_loop(o, mqtt, cry_log.get(), on_cry_event); });

  using clock = std::chrono::steady_clock;
  auto next_tick = clock::now() + std::chrono::seconds(1);
  auto next_report = clock::now() + std::chrono::seconds(60);
  bool radar_waiting = false;
  while (!g_stop) {
    const auto now = clock::now();
    const int wait_ms = static_cast<int>(
        std::max<long>(0, std::min<long>(200, std::chrono::duration_cast<std::chrono::milliseconds>(next_tick - now).count())));
    mqtt.poll(wait_ms);
    if (clock::now() >= next_tick) {
      next_tick += std::chrono::seconds(1);
      if (clock::now() - next_tick > std::chrono::seconds(2)) next_tick = clock::now() + std::chrono::seconds(1);
      if (o.fusion) {
        fusion.tick();
#ifdef NUNI_WITH_DBUS
        if(dbus) {
          const auto sensors=fusion.current_sensors();
          for(const char* key:{"radar","cry","env"}) dbus->set_sensor(key,sensors[key]);
        }
#endif
      }
      if (auto result=voice.completion()) {
        mqtt.publish(topics::SOOTHE,dump(Json{{"id",pyget(playing_event,"id")},
          {"status",*result==0?"completed":"failed"},{"exit_code",*result},{"ts",wall_now()}}));
      }
      if (o.radar) {
        if (auto m = radar.step(steady_now())) {
          mqtt.publish(topics::RADAR, dump(*m));
          ++n_radar_out;
          radar_waiting = false;
        } else if (!radar_waiting) {
          std::cerr << "[radar] ESPHome 수신 대기 중 (레이더 미연결/stale)\n";
          radar_waiting = true;
        }
      }
    }
    if (clock::now() >= next_report) {
      next_report += std::chrono::seconds(60);
      std::cerr << "[nuni] 1분 요약: MQTT " << (mqtt.connected() ? "접속" : "끊김") << ", 받은 메시지 " << n_in
                << ", 레이더 발행 " << n_radar_out << ", 상태 발행 " << n_state << "\n";
    }
  }

  std::cerr << "[nuni] 종료 중...\n";
  if (cry_th.joinable()) cry_th.join();
  return 0;
}
