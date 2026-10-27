#include "nuni/cry_log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>

namespace fs = std::filesystem;

namespace nuni {

namespace {

constexpr double kCapH = 12.0;  // cry_personal.CAP_H
constexpr double kPi = 3.14159265358979323846;
constexpr int kSr = 16000;

std::string local_id(double t) {
  const std::time_t s = static_cast<std::time_t>(t);
  std::tm tm{};
  localtime_r(&s, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return buf;
}

Json opt_num(const std::optional<double>& v) { return v ? Json(*v) : Json(nullptr); }

int16_t to_pcm16(float x) {
  const float c = std::max(-1.0f, std::min(1.0f, x));
  return static_cast<int16_t>(std::lround(c * 32767.0f));
}

bool write_wav(const std::string& path, const std::vector<int16_t>& pcm) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  const uint32_t data = static_cast<uint32_t>(pcm.size() * 2);
  f.write("RIFF", 4);
  u32(36 + data);
  f.write("WAVEfmt ", 8);
  u32(16);
  u16(1);        // PCM
  u16(1);        // 모노
  u32(kSr);
  u32(kSr * 2);  // 바이트/초
  u16(2);
  u16(16);
  f.write("data", 4);
  u32(data);
  f.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(data));
  return static_cast<bool>(f);
}

std::vector<std::string> str_list(const Json& v) {
  std::vector<std::string> out;
  if (v.is_array())
    for (const auto& x : v)
      if (x.is_string()) out.push_back(x.get<std::string>());
  return out;
}

}  // namespace

// ── 맥락 ─────────────────────────────────────────────────────────────────────

std::optional<CryContextModel> load_cry_context(const std::string& path) {
  std::ifstream f(path);
  if (!f) return std::nullopt;
  std::stringstream ss;
  ss << f.rdbuf();
  const Json j = Json::parse(ss.str(), nullptr, false);
  if (j.is_discarded() || !j.is_object()) return std::nullopt;
  CryContextModel m;
  try {
    for (const auto& row : j.at("coef")) m.coef.push_back(row.get<std::vector<double>>());
    m.intercept = j.at("intercept").get<std::vector<double>>();
    m.mean = j.at("mean").get<std::vector<double>>();
    m.std = j.at("std").get<std::vector<double>>();
    m.classes = j.contains("classes") ? str_list(j["classes"]) : std::vector<std::string>{"feed", "diaper", "hold"};
  } catch (const std::exception&) {
    return std::nullopt;
  }
  if (m.coef.size() != m.classes.size() || m.intercept.size() != m.classes.size() || m.mean.size() != 4 ||
      m.std.size() != 4)
    return std::nullopt;
  for (const auto& row : m.coef)
    if (row.size() != 4) return std::nullopt;
  return m;
}

std::array<double, 4> cry_ctx_features(double t, std::optional<double> last_cry, std::optional<double> last_feed) {
  const std::time_t s = static_cast<std::time_t>(t);
  std::tm tm{};
  localtime_r(&s, &tm);
  const double h = tm.tm_hour + tm.tm_min / 60.0;
  auto since = [&](const std::optional<double>& p) {
    return p ? std::min(std::max((t - *p) / 3600.0, 0.0), kCapH) : kCapH;
  };
  return {std::sin(2 * kPi * h / 24), std::cos(2 * kPi * h / 24), since(last_cry), since(last_feed)};
}

std::string cry_label_ko(const std::string& a) {
  if (a == "feed") return "수유";
  if (a == "diaper") return "기저귀";
  if (a == "sleep") return "재우기";
  if (a == "hold") return "안아주기·자세 바꾸기";
  if (a == "burp") return "트림·가스 빼기";
  if (a == "temp") return "온도·환경 조절";
  if (a == "self") return "저절로 그침";
  if (a == "unknown") return "모름";
  if (a == "other") return "기타";
  if (a == "sick") return "아파 보임";
  return a;
}

// ── CryLog ───────────────────────────────────────────────────────────────────

CryLog::CryLog(CryLogConfig cfg) : cfg_(std::move(cfg)) {
  if (!cfg_.ctx_path.empty()) {
    ctx_ = load_cry_context(cfg_.ctx_path);
    if (!ctx_) std::cerr << "[crylog] 맥락 모델을 읽지 못함(" << cfg_.ctx_path << ") — 맥락은 균등으로 둔다\n";
  }
  std::error_code ec;
  fs::create_directories(cfg_.dir, ec);
  if (cfg_.record) {
    fs::create_directories(fs::path(cfg_.dir) / "audio", ec);
    fs::create_directories(fs::path(cfg_.dir) / "emb", ec);
  }
  if (ec) std::cerr << "[crylog] 기록 폴더를 만들지 못함(" << cfg_.dir << "): " << ec.message() << "\n";
  load_();
  std::cerr << "[crylog] " << cfg_.dir << " — 울음 " << episodes_.size() << "개, 맥락 " << (ctx_ ? "공통 모델" : "균등")
            << ", 소리 저장 " << (cfg_.record ? "켬" : "끔") << "\n";
}

CryLog::~CryLog() {
  std::lock_guard<std::mutex> lk(mu_);
  if (cur_ && cur_->emb_fp) std::fclose(cur_->emb_fp);
}

bool CryLog::valid_answer_(const std::string& a) const {
  if (std::find(cfg_.actions.begin(), cfg_.actions.end(), a) != cfg_.actions.end()) return true;
  const auto& ex = cry_extra_answers();
  return std::find(ex.begin(), ex.end(), a) != ex.end();
}

std::optional<double> CryLog::last_cry_() const {
  if (episodes_.empty()) return std::nullopt;
  return episodes_.back().t1;
}

// cry_personal._context: 공통 맥락 모델(수유 / 기저귀 / 안아주기로 학습). 그 밖의 행동은 세 확률의 평균.
std::vector<double> CryLog::context_(double t, std::optional<double> last_cry) const {
  const size_t K = cfg_.actions.size();
  if (!ctx_) return std::vector<double>(K, 1.0 / K);
  const auto f = cry_ctx_features(t, last_cry, last_feed_);
  const size_t C = ctx_->classes.size();
  std::vector<double> z(C);
  for (size_t k = 0; k < C; ++k) {
    double v = ctx_->intercept[k];
    for (size_t i = 0; i < 4; ++i) v += ctx_->coef[k][i] * ((f[i] - ctx_->mean[i]) / ctx_->std[i]);
    z[k] = v;
  }
  const double zmax = *std::max_element(z.begin(), z.end());
  std::vector<double> pc(C);
  double s = 0;
  for (size_t k = 0; k < C; ++k) s += (pc[k] = std::exp(z[k] - zmax));
  for (auto& v : pc) v /= s;
  const double pc_mean = std::accumulate(pc.begin(), pc.end(), 0.0) / C;
  std::vector<double> p(K);
  double ps = 0;
  for (size_t k = 0; k < K; ++k) {
    auto it = std::find(ctx_->classes.begin(), ctx_->classes.end(), cfg_.actions[k]);
    p[k] = it != ctx_->classes.end() ? pc[it - ctx_->classes.begin()] : pc_mean;
    ps += p[k];
  }
  for (auto& v : p) v /= ps;
  return p;
}

// cry_personal._prior: 이 아기의 이전 답 빈도. 최근 답일수록 크게(half_life개마다 절반), 라플라스 +1.
std::vector<double> CryLog::prior_() const {
  const size_t K = cfg_.actions.size();
  std::vector<size_t> lab;  // 답(학습하는 행동) 순서대로
  for (const auto& e : episodes_) {
    auto it = std::find(cfg_.actions.begin(), cfg_.actions.end(), e.action);
    if (it != cfg_.actions.end()) lab.push_back(static_cast<size_t>(it - cfg_.actions.begin()));
  }
  const size_t n = lab.size();
  std::vector<double> cnt(K, 0.0);
  for (size_t i = 0; i < n; ++i) {
    const double w = (cfg_.half_life > 0) ? std::pow(0.5, static_cast<double>(n - 1 - i) / cfg_.half_life) : 1.0;
    cnt[lab[i]] += w;
  }
  const double tot = std::accumulate(cnt.begin(), cnt.end(), 0.0);
  std::vector<double> p(K);
  for (size_t k = 0; k < K; ++k) p[k] = (cnt[k] + 1) / (tot + static_cast<double>(K));
  return p;
}

// cry_personal.hint(t, embs=[]): 맥락 × 이력(소리 없음)
Json CryLog::hint_(double t) const {
  const size_t K = cfg_.actions.size();
  const auto p_ctx = context_(t, last_cry_());
  const auto p_prior = prior_();
  size_t n_lab = 0;
  for (const auto& e : episodes_)
    if (std::find(cfg_.actions.begin(), cfg_.actions.end(), e.action) != cfg_.actions.end()) ++n_lab;
  std::vector<double> p(K);
  double s = 0;
  for (size_t k = 0; k < K; ++k) s += (p[k] = p_ctx[k] * p_prior[k]);
  for (auto& v : p) v /= s;
  std::vector<size_t> order(K);
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return p[a] > p[b]; });
  const size_t top = order[0];
  const double top2_sum = K >= 2 ? p[order[0]] + p[order[1]] : p[order[0]];

  Json probs = Json::object();
  for (size_t k = 0; k < K; ++k) probs[cfg_.actions[k]] = py_round(p[k], 3);
  Json top2 = Json::array(), top2_ko = Json::array();
  for (size_t i = 0; i < std::min<size_t>(2, K); ++i) {
    top2.push_back(cfg_.actions[order[i]]);
    top2_ko.push_back(cry_label_ko(cfg_.actions[order[i]]));
  }
  auto r3 = [](const std::vector<double>& v) {
    Json a = Json::array();
    for (double x : v) a.push_back(py_round(x, 3));
    return a;
  };
  Json h;
  h["probs"] = probs;
  h["top"] = cfg_.actions[top];
  h["top_ko"] = cry_label_ko(cfg_.actions[top]);
  h["confidence"] = py_round(p[top], 3);
  h["top2"] = top2;
  h["top2_ko"] = top2_ko;
  h["top2_conf"] = py_round(top2_sum, 3);
  const bool enough = n_lab >= static_cast<size_t>(cfg_.k_min);
  h["show"] = enough && p[top] >= cfg_.show_min;
  h["show2"] = enough && top2_sum >= cfg_.show2_min;
  h["n_feedback"] = n_lab;
  h["hours_since_feed"] = last_feed_ ? Json(py_round((t - *last_feed_) / 3600.0, 1)) : Json(nullptr);
  h["sound_on"] = false;  // 소리 비교는 아직 옮기지 않음(자가 검증이 소리를 끈 상태와 같다)
  h["recent"] = Json{{"n", 0}, {"hit_sound", 0}, {"hit_base", 0}, {"sound_better", false}};
  h["parts"] = Json{{"context", r3(p_ctx)}, {"history", r3(p_prior)}};
  return h;
}

Json CryLog::hint(double t) const {
  std::lock_guard<std::mutex> lk(mu_);
  return hint_(t);
}

// ── 울음 구간 ─────────────────────────────────────────────────────────────────

std::vector<CryLog::Event> CryLog::tick(double t, double score, const std::vector<float>& emb,
                                        const std::vector<float>& audio16k) {
  std::vector<Event> evs;
  const bool crying = score >= cfg_.min_score;
  std::unique_lock<std::mutex> lk(mu_);
  if (!cur_) {
    if (!crying) {  // 울음 전 소리는 preroll로만 잠깐 들고 있는다
      if (cfg_.record) {
        for (float x : audio16k) preroll_.push_back(to_pcm16(x));
        const size_t cap = static_cast<size_t>(cfg_.preroll_s * kSr);
        while (preroll_.size() > cap) preroll_.pop_front();
      }
      return evs;
    }
    evs = start_(t);
  }
  if (crying) {
    cur_->last_cry = t;
    add_window_(emb, audio16k, score);
  } else {
    append_audio_(audio16k);  // 울음 사이 조용한 1초도 소리는 이어 붙인다
  }
  if (!crying && t - cur_->last_cry >= cfg_.end_gap_s) {
    if (auto e = end_(cur_->last_cry)) evs.push_back(*e);
  }
  return evs;
}

std::vector<CryLog::Event> CryLog::start(double t) {
  std::lock_guard<std::mutex> lk(mu_);
  return start_(t);
}

std::vector<CryLog::Event> CryLog::start_(double t) {
  std::vector<Event> evs;
  if (cur_) {  // 앞 울음이 아직 열려 있으면 닫고 시작한다
    if (auto e = end_(cur_->last_cry)) evs.push_back(*e);
  }
  Current c;
  c.id = local_id(t);
  for (const auto& e : episodes_)  // 같은 초에 끝나고 다시 시작하는 경우(시험 재생)
    if (e.id == c.id) c.id += "b";
  c.t0 = t;
  c.last_cry = t;
  c.audio.assign(preroll_.begin(), preroll_.end());
  preroll_.clear();
  c.hint = hint_(t);
  cur_ = std::move(c);
  Json h = cur_->hint;
  Json start{{"event", "start"}, {"id", cur_->id}, {"t0", t}, {"hint", h}};
  evs.push_back({"start", start});
  Json hint_ev{{"event", "hint"}, {"id", cur_->id}};
  for (auto it = h.begin(); it != h.end(); ++it) hint_ev[it.key()] = it.value();
  evs.push_back({"hint", hint_ev});
  return evs;
}

void CryLog::add_window(const std::vector<float>& emb, const std::vector<float>& audio16k, double score) {
  std::lock_guard<std::mutex> lk(mu_);
  add_window_(emb, audio16k, score);
}

void CryLog::add_window_(const std::vector<float>& emb, const std::vector<float>& audio16k, double score) {
  if (!cur_) return;
  ++cur_->n;
  cur_->peak = std::max(cur_->peak, score);
  cur_->sum += score;
  if (cfg_.record && emb.size() == 1024) {
    if (!cur_->emb_fp) {
      const auto p = fs::path(cfg_.dir) / "emb" / (cur_->id + ".f32");
      cur_->emb_fp = std::fopen(p.c_str(), "wb");
    }
    if (cur_->emb_fp) std::fwrite(emb.data(), sizeof(float), emb.size(), cur_->emb_fp);
  }
  append_audio_(audio16k);
}

void CryLog::append_audio_(const std::vector<float>& audio16k) {
  if (!cfg_.record || !cur_) return;
  const size_t cap = static_cast<size_t>(cfg_.max_audio_s * kSr);
  for (float x : audio16k) {
    if (cur_->audio.size() >= cap) break;
    cur_->audio.push_back(to_pcm16(x));
  }
}

std::optional<CryLog::Event> CryLog::end(double t) {
  std::lock_guard<std::mutex> lk(mu_);
  return end_(t);
}

std::optional<CryLog::Event> CryLog::end_(double t) {
  if (!cur_) return std::nullopt;
  Current c = std::move(*cur_);
  cur_.reset();
  if (c.emb_fp) std::fclose(c.emb_fp);
  const auto emb_path = fs::path(cfg_.dir) / "emb" / (c.id + ".f32");
  if (c.n < cfg_.min_windows) {  // 짧은 잡음 — 기록하지 않는다
    std::error_code ec;
    fs::remove(emb_path, ec);
    return Event{"drop", Json{{"event", "drop"}, {"id", c.id}, {"t0", c.t0}, {"n", c.n}}};
  }
  Episode e;
  e.id = c.id;
  e.t0 = c.t0;
  e.t1 = t;
  e.n = c.n;
  e.peak = py_round(c.peak, 3);
  e.mean = py_round(c.sum / c.n, 3);
  e.nots = str_list(c.hint.value("not", Json::array()));  // 울음 중에 누른 '해 봤는데 안 그쳤어요'
  // cry_personal.end: 답을 받기 전에 '맥락 × 이력' 예측과 울음 시작 때 맥락 특징을 남긴다
  const auto last_cry = last_cry_();
  e.c = cry_ctx_features(c.t0, last_cry, last_feed_);
  const auto pc = context_(c.t0, last_cry);
  const auto pp = prior_();
  size_t best = 0;
  for (size_t k = 1; k < pc.size(); ++k)
    if (pc[k] * pp[k] > pc[best] * pp[best]) best = k;
  e.pred_base = cfg_.actions[best];
  e.hint = Json{{"top", c.hint.value("top", "")},
                {"top2", c.hint.value("top2", Json::array())},
                {"probs", c.hint.value("probs", Json::object())},
                {"show", c.hint.value("show", false)},
                {"show2", c.hint.value("show2", false)}};
  if (cfg_.record) {
    if (!c.audio.empty()) {
      const auto rel = fs::path("audio") / (c.id + ".wav");
      if (write_wav((fs::path(cfg_.dir) / rel).string(), c.audio)) e.audio = rel.string();
    }
    std::error_code ec;
    if (fs::exists(emb_path, ec)) e.emb = (fs::path("emb") / (c.id + ".f32")).string();
  }
  episodes_.push_back(e);
  save_();
  if (cfg_.record) prune_audio_();
  Json ev = episode_json_(episodes_.back(), false);
  ev["event"] = "end";
  Json cf = Json::array();
  for (double v : e.c) cf.push_back(py_round(v, 4));
  ev["c"] = cf;
  return Event{"end", ev};
}

// ── 부모 답 ──────────────────────────────────────────────────────────────────

CryLog::Episode* CryLog::find_(const std::string& id) {
  for (auto it = episodes_.rbegin(); it != episodes_.rend(); ++it)
    if (it->id == id) return &*it;
  return nullptr;
}

std::optional<CryLog::Event> CryLog::feedback(const std::string& id, const std::string& action, double now,
                                              std::string* error) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!valid_answer_(action)) {
    if (error) *error = "알 수 없는 답: " + action;
    return std::nullopt;
  }
  Episode* e = find_(id);
  if (!e) {
    if (error) *error = (cur_ && cur_->id == id) ? "울음이 끝난 뒤에 답할 수 있어요" : "그 울음 기록이 없어요: " + id;
    return std::nullopt;
  }
  e->action = action;
  e->answered_at = now;
  if (action == "feed") last_feed_ = std::max(last_feed_.value_or(0.0), e->t1);  // cry_personal.feedback
  save_();
  return Event{"answer", Json{{"event", "answer"}, {"id", id}, {"action", action}, {"label", cry_label_ko(action)},
                              {"ts", now}, {"last_feed", opt_num(last_feed_)}}};
}

std::optional<CryLog::Event> CryLog::feedback_not(const std::string& id, const std::string& action, double now,
                                                  std::string* error) {
  std::lock_guard<std::mutex> lk(mu_);
  if (std::find(cfg_.actions.begin(), cfg_.actions.end(), action) == cfg_.actions.end()) {
    if (error) *error = "'안 그쳤어요'는 수유·기저귀·안아주기에만 쓸 수 있어요: " + action;
    return std::nullopt;
  }
  // 울음 중에 누른 '해 봤는데 안 그쳤어요'는 울음이 끝날 때 붙인다
  if (cur_ && cur_->id == id) {
    Json& h = cur_->hint;
    Json nots = h.value("not", Json::array());
    if (std::find(nots.begin(), nots.end(), Json(action)) == nots.end()) nots.push_back(action);
    h["not"] = nots;
    return Event{"not", Json{{"event", "not"}, {"id", id}, {"action", action}, {"label", cry_label_ko(action)},
                             {"ts", now}, {"not", nots}}};
  }
  Episode* e = find_(id);
  if (!e) {
    if (error) *error = "그 울음 기록이 없어요: " + id;
    return std::nullopt;
  }
  if (std::find(e->nots.begin(), e->nots.end(), action) == e->nots.end()) e->nots.push_back(action);
  save_();
  Json nots = e->nots;
  return Event{"not", Json{{"event", "not"}, {"id", id}, {"action", action}, {"label", cry_label_ko(action)},
                           {"ts", now}, {"not", nots}}};
}

CryLog::Event CryLog::log_feed(double t) {
  std::lock_guard<std::mutex> lk(mu_);
  last_feed_ = std::max(last_feed_.value_or(0.0), t);  // cry_personal.log_feed
  save_();
  return Event{"feed", Json{{"event", "feed"}, {"ts", t}, {"last_feed", *last_feed_}}};
}

// ── 보기 ─────────────────────────────────────────────────────────────────────

Json CryLog::episode_json_(const Episode& e, bool full) const {
  Json j;
  j["id"] = e.id;
  j["t0"] = e.t0;
  j["t1"] = e.t1;
  j["n"] = e.n;
  j["peak"] = e.peak;
  j["mean"] = e.mean;
  j["action"] = e.action.empty() ? Json(nullptr) : Json(e.action);
  j["not"] = e.nots;
  j["pred_base"] = e.pred_base;
  j["hint"] = e.hint;
  j["answered_at"] = e.answered_at > 0 ? Json(e.answered_at) : Json(nullptr);
  if (full) {
    Json c = Json::array();
    for (double v : e.c) c.push_back(py_round(v, 4));
    j["c"] = c;
    j["audio"] = e.audio.empty() ? Json(nullptr) : Json(e.audio);
    j["emb"] = e.emb.empty() ? Json(nullptr) : Json(e.emb);
  } else {
    j["has_audio"] = !e.audio.empty();
  }
  return j;
}

Json CryLog::snapshot(double now, size_t recent) const {
  std::lock_guard<std::mutex> lk(mu_);
  Json s;
  s["actions"] = cfg_.actions;
  Json answers = cfg_.actions;
  for (const auto& a : cry_extra_answers()) answers.push_back(a);
  s["answers"] = answers;
  Json labels = Json::object();
  for (const auto& a : answers) labels[a.get<std::string>()] = cry_label_ko(a.get<std::string>());
  s["labels"] = labels;
  s["k_min"] = cfg_.k_min;
  size_t n_lab = 0, n_ans = 0;
  for (const auto& e : episodes_) {
    if (!e.action.empty()) ++n_ans;
    if (std::find(cfg_.actions.begin(), cfg_.actions.end(), e.action) != cfg_.actions.end()) ++n_lab;
  }
  s["n_labeled"] = n_lab;
  s["n_answered"] = n_ans;
  s["n_episodes"] = episodes_.size();
  s["last_feed"] = opt_num(last_feed_);
  if (cur_) {
    s["current"] = Json{{"id", cur_->id}, {"t0", cur_->t0}, {"seconds", std::max(0.0, now - cur_->t0)},
                        {"n", cur_->n}, {"peak", py_round(cur_->peak, 3)}, {"hint", cur_->hint}};
  } else {
    s["current"] = nullptr;
  }
  // 가장 최근에 끝났고 아직 답이 없는 울음(오래된 것은 묻지 않는다)
  s["pending"] = nullptr;
  if (!episodes_.empty()) {
    const auto& last = episodes_.back();
    if (last.action.empty() && now - last.t1 <= cfg_.pending_hours * 3600) s["pending"] = last.id;
  }
  // 최근 답에서 울음 시작 때 낸 힌트가 맞은 수(1순위 / 2순위 안)
  int n = 0, hit1 = 0, hit2 = 0;
  for (auto it = episodes_.rbegin(); it != episodes_.rend() && n < cfg_.check_window; ++it) {
    if (std::find(cfg_.actions.begin(), cfg_.actions.end(), it->action) == cfg_.actions.end()) continue;
    if (!it->hint.is_object() || !it->hint.contains("top")) continue;
    ++n;
    if (it->hint.value("top", "") == it->action) ++hit1;
    const Json t2 = it->hint.value("top2", Json::array());
    if (std::find(t2.begin(), t2.end(), Json(it->action)) != t2.end()) ++hit2;
  }
  s["hits"] = Json{{"n", n}, {"top1", hit1}, {"top2", hit2}};
  Json eps = Json::array();
  const size_t from = episodes_.size() > recent ? episodes_.size() - recent : 0;
  for (size_t i = from; i < episodes_.size(); ++i) eps.push_back(episode_json_(episodes_[i], false));
  s["episodes"] = eps;
  s["recording"] = cfg_.record;
  s["sound_model"] = false;
  return s;
}

// ── 저장 ─────────────────────────────────────────────────────────────────────

void CryLog::save_() const {
  Json d;
  d["version"] = 1;
  d["actions"] = cfg_.actions;
  d["last_feed"] = opt_num(last_feed_);
  Json eps = Json::array();
  for (const auto& e : episodes_) eps.push_back(episode_json_(e, true));
  d["episodes"] = eps;
  const auto path = fs::path(cfg_.dir) / "cry_log.json";
  const auto tmp = fs::path(cfg_.dir) / "cry_log.json.tmp";
  {
    std::ofstream f(tmp);
    if (!f) {
      std::cerr << "[crylog] 저장 실패: " << tmp << "\n";
      return;
    }
    f << dump(d) << "\n";
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);  // 같은 파티션 안 rename이라 중간에 끊겨도 앞 파일이 남는다
  if (ec) std::cerr << "[crylog] 저장 실패(rename): " << ec.message() << "\n";
}

void CryLog::load_() {
  const auto path = fs::path(cfg_.dir) / "cry_log.json";
  std::ifstream f(path);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  const Json d = Json::parse(ss.str(), nullptr, false);
  if (d.is_discarded() || !d.is_object()) {
    const auto bad = fs::path(cfg_.dir) / ("cry_log.broken-" + local_id(wall_now()) + ".json");
    std::error_code ec;
    fs::rename(path, bad, ec);
    std::cerr << "[crylog] cry_log.json을 읽지 못해 " << bad << "로 옮기고 새로 시작한다\n";
    return;
  }
  const Json lf = d.value("last_feed", Json(nullptr));
  if (lf.is_number()) last_feed_ = lf.get<double>();
  for (const auto& j : d.value("episodes", Json::array())) {
    if (!j.is_object() || !j.contains("id")) continue;
    Episode e;
    e.id = j.value("id", "");
    e.t0 = j.value("t0", 0.0);
    e.t1 = j.value("t1", 0.0);
    e.n = j.value("n", 0);
    e.peak = j.value("peak", 0.0);
    e.mean = j.value("mean", 0.0);
    const Json a = j.value("action", Json(nullptr));
    if (a.is_string()) e.action = a.get<std::string>();
    e.nots = str_list(j.value("not", Json::array()));
    const Json c = j.value("c", Json::array());
    for (size_t i = 0; i < 4 && i < c.size(); ++i) e.c[i] = c[i].is_number() ? c[i].get<double>() : 0.0;
    e.pred_base = j.value("pred_base", "");
    e.hint = j.value("hint", Json(nullptr));
    const Json at = j.value("answered_at", Json(nullptr));
    if (at.is_number()) e.answered_at = at.get<double>();
    const Json au = j.value("audio", Json(nullptr)), em = j.value("emb", Json(nullptr));
    if (au.is_string()) e.audio = au.get<std::string>();
    if (em.is_string()) e.emb = em.get<std::string>();
    episodes_.push_back(std::move(e));
  }
}

void CryLog::prune_audio_() {
  const auto dir = fs::path(cfg_.dir) / "audio";
  std::error_code ec;
  if (!fs::exists(dir, ec)) return;
  std::vector<std::pair<std::string, uintmax_t>> files;
  uintmax_t total = 0;
  for (const auto& ent : fs::directory_iterator(dir, ec)) {
    if (!ent.is_regular_file(ec) || ent.path().extension() != ".wav") continue;
    const auto sz = ent.file_size(ec);
    files.emplace_back(ent.path().filename().string(), sz);
    total += sz;
  }
  const auto cap = static_cast<uintmax_t>(cfg_.max_audio_mb * 1024 * 1024);
  if (total <= cap) return;
  std::sort(files.begin(), files.end());  // 이름 = 시작 시각이라 앞쪽이 오래된 것
  bool changed = false;
  for (const auto& [name, sz] : files) {
    if (total <= cap) break;
    fs::remove(dir / name, ec);
    total -= sz;
    const std::string rel = (fs::path("audio") / name).string();
    for (auto& e : episodes_)
      if (e.audio == rel) {
        e.audio.clear();
        changed = true;
      }
  }
  if (changed) save_();
}

}  // namespace nuni
