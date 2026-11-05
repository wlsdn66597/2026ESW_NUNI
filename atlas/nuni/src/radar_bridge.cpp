#include "nuni/radar_bridge.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "nuni/topics.hpp"

namespace nuni {

namespace {

double env_double(const char* name, double def) {
  const char* v = std::getenv(name);
  if (!v) return def;
  auto p = parse_py_float(v);
  return p ? *p : def;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

Json opt_json(const std::optional<double>& v) { return v ? Json(*v) : Json(nullptr); }  // NaN은 dump에서 null

}  // namespace

std::optional<double> parse_py_float(const std::string& raw) {
  const std::string s = trim(raw);
  if (s.empty()) return std::nullopt;
  // Python float()은 16진수("0x1A")를 받지 않는다
  for (size_t i = 0; i + 1 < s.size(); ++i)
    if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) return std::nullopt;
  if (s.find('(') != std::string::npos) return std::nullopt;  // strtod의 "nan(...)" 형식 배제
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end != s.c_str() + s.size()) return std::nullopt;
  return v;
}

RadarConfig RadarConfig::from_env() {
  RadarConfig c;
  if (const char* p = std::getenv("NUNI_ESPHOME_PREFIX")) c.prefix = p;
  c.apnea_hold_s = env_double("NUNI_APNEA_HOLD_S", c.apnea_hold_s);
  c.apnea_low_br = env_double("NUNI_APNEA_LOW_BR", c.apnea_low_br);
  c.apnea_require_prior_s = env_double("NUNI_APNEA_PRIOR_S", c.apnea_require_prior_s);
  c.stale_s = env_double("NUNI_RADAR_STALE_S", c.stale_s);
  c.move_window_s = env_double("NUNI_MOVE_WINDOW_S", c.move_window_s);
  c.move_scale_cm = env_double("NUNI_MOVE_SCALE_CM", c.move_scale_cm);
  c.br_min = env_double("NUNI_BR_MIN", c.br_min);
  c.br_max = env_double("NUNI_BR_MAX", c.br_max);
  c.dist_min_cm = env_double("NUNI_DIST_MIN_CM", c.dist_min_cm);
  c.dist_max_cm = env_double("NUNI_DIST_MAX_CM", c.dist_max_cm);
  c.move_reject = env_double("NUNI_MOVE_REJECT", c.move_reject);
  c.smooth_window_s = env_double("NUNI_BR_SMOOTH_S", c.smooth_window_s);
  c.tof_leave_mm = env_double("NUNI_TOF_LEAVE_MM", c.tof_leave_mm);
  c.tof_stale_s = env_double("NUNI_TOF_STALE_S", c.tof_stale_s);
  return c;
}

RadarBridge::RadarBridge(RadarConfig cfg) : cfg_(std::move(cfg)) {}

std::vector<std::string> RadarBridge::esphome_topics() const {
  return {cfg_.prefix + "/breath_rate", cfg_.prefix + "/heart_rate", cfg_.prefix + "/presence",
          cfg_.prefix + "/distance", cfg_.prefix + "/num_targets", cfg_.prefix + "/status"};
}

void RadarBridge::on_esphome(const std::string& topic, const std::string& raw, double now) {
  const std::string& p = cfg_.prefix;
  if (topic == p + "/status") {
    if (trim(raw) != "offline") return;
    // An explicit disconnect invalidates every field immediately.
    breath_.reset(); heart_.reset(); distance_.reset(); presence_.reset();
    breath_rx_=heart_rx_=distance_rx_=presence_rx_=-1;
    dist_hist_.clear(); br_hist_.clear(); last_valid_.reset(); stop_low_breath();
    return;
  } else if (topic == p + "/num_targets") {
    // ESPHome presence is published on change; target count is periodic.
    // It refreshes presence only, never the age of a breath measurement.
    const auto count=parse_py_float(raw);
    if(!count || !std::isfinite(*count) || *count<0 || std::floor(*count)!=*count) return;
    presence_=*count>0; presence_rx_=now;
    // 대상이 사라지면 표시용 창은 비운다. 이미 시작된 무호흡 카운트는 멈추지 않는다
    // (숨이 멈추면 레이더가 대상을 놓칠 수 있다 — 이탈 여부는 ToF로 본다).
    if(!*presence_) { br_hist_.clear(); last_valid_.reset(); }
  } else if (topic == p + "/presence") {
    std::string s = trim(raw);
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if(s!="ON" && s!="TRUE" && s!="1" && s!="OFF" && s!="FALSE" && s!="0") return;
    presence_ = (s == "ON" || s == "TRUE" || s == "1");
    presence_rx_ = now;
  } else {
    std::optional<double>* slot = nullptr;
    if (topic == p + "/breath_rate") slot = &breath_;        // 회/분
    else if (topic == p + "/heart_rate") slot = &heart_;     // 참고 기록용
    else if (topic == p + "/distance") slot = &distance_;    // cm (움직임 유도용)
    else return;                                             // 모르는 토픽
    const auto val = parse_py_float(raw);
    if (!val) return;                                        // 숫자가 아니면 무시(last_rx도 그대로)
    *slot = *val;
    if (slot == &breath_) { breath_rx_ = now; ++breath_seq_; }
    if (slot == &heart_) heart_rx_ = now;
    if (slot == &distance_) distance_rx_ = now;
    // 0.00은 '대상 없음'이지 위치가 아니므로 움직임 계산에 넣지 않는다.
    if (slot == &distance_ && !std::isnan(*val) && *val > 0) dist_hist_.emplace_back(now, *val);
  }
  last_rx_ = now;
}

double RadarBridge::movement(double now) {
  while (!dist_hist_.empty() && now - dist_hist_.front().first > cfg_.move_window_s) dist_hist_.pop_front();
  if (dist_hist_.size() < 2) return 0.1;
  double lo = dist_hist_.front().second, hi = lo;
  for (const auto& [t, d] : dist_hist_) {
    lo = std::min(lo, d);
    hi = std::max(hi, d);
  }
  return std::max(0.0, std::min(1.0, (hi - lo) / cfg_.move_scale_cm));
}

bool RadarBridge::in_range(std::optional<double> dist) const {
  return dist && !std::isnan(*dist) && cfg_.dist_min_cm < *dist && *dist <= cfg_.dist_max_cm;
}

bool RadarBridge::breath_is_trustworthy(std::optional<double> bpm, std::optional<double> dist, bool presence,
                                        double move) const {
  if (!presence || !bpm || std::isnan(*bpm)) return false;
  if (!(cfg_.br_min <= *bpm && *bpm <= cfg_.br_max)) return false;  // 0·튄 값 배제
  if (!in_range(dist)) return false;                                 // 유효 거리 밖이면 호흡값 무의미
  return move <= cfg_.move_reject;                                    // 크게 움직이면 호흡 신호가 묻힘
}

void RadarBridge::on_tof(std::optional<double> distance_mm, double now) {
  tof_rx_ = now;
  if (distance_mm && std::isfinite(*distance_mm) && *distance_mm > 0) tof_mm_ = *distance_mm;
  else tof_mm_.reset();
}

std::optional<double> RadarBridge::smoothed_breath(double now) {
  while (!br_hist_.empty() && now - br_hist_.front().first > cfg_.smooth_window_s) br_hist_.pop_front();
  if (br_hist_.empty()) return std::nullopt;
  std::vector<double> v;
  v.reserve(br_hist_.size());
  for (const auto& [t, b] : br_hist_) v.push_back(b);
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;  // statistics.median
}

void RadarBridge::stop_low_breath() {
  low_since_.reset();
  low_baseline_.reset();
  tof_ref_.reset();
}

void RadarBridge::update_low_breath(std::optional<double> raw, bool presence, std::optional<double> dist,
                                    bool movement_valid, double move, double now) {
  // 원본 호흡값이 apnea_low_br 이하인 시간을 잰다. MR60은 호흡이 정상이어도 값이 1씩 미끄러져
  // 1~4까지 내려갔다가 6~7초 안에 돌아오는 구간이 있다(2026-11-04 실측). 그래서 순간값이 아니라
  // 지속 시간(apnea_hold_s)으로 판정한다. 레이더 수신이 끊기면(raw 없음) 셀 수 없으므로 멈춘다.
  const bool low = raw && !std::isnan(*raw) && *raw <= cfg_.apnea_low_br;
  if (!low) { stop_low_breath(); return; }
  const bool tof_known = tof_mm_ && tof_rx_ >= 0 && now >= tof_rx_ && now - tof_rx_ <= cfg_.tof_stale_s;
  if (!low_since_) {
    // 시작 조건: 재실·유효 거리·정지, 그리고 직전에 유효 호흡을 본 적이 있음(미측정과 구분).
    const bool had_baseline = last_valid_ && (now - *last_valid_) <= cfg_.apnea_require_prior_s;
    if (presence && in_range(dist) && movement_valid && move <= cfg_.move_reject && had_baseline) {
      low_since_ = now;
      low_baseline_ = last_valid_;
      if (tof_known) tof_ref_ = tof_mm_;
    }
    return;
  }
  // 마지막 유효 호흡 후 apnea_require_prior_s가 지나면 판정을 끝낸다(예전 동작). 아무것도 없는데
  // 레이더가 정지 물체를 대상으로 잡으면 경보가 끝없이 이어지던 문제(2026-11-05 실측 11분)를 막는다.
  // 유효 호흡을 다시 보기 전에는 새 카운트도 시작하지 않는다(시작 조건의 had_baseline).
  if (low_baseline_ && now - *low_baseline_ > cfg_.apnea_require_prior_s) { stop_low_breath(); return; }
  // 카운트 중: 재실이 꺼져도 계속 센다. 난간 ToF가 크게 바뀌면(안아 올림·일어남) 이탈로 보고 멈춘다.
  // ToF가 무효·오래됨이면 변화를 알 수 없으므로 계속 센다(놓치는 것보다 오경보가 낫다).
  if (tof_known) {
    if (!tof_ref_) tof_ref_ = tof_mm_;
    else if (std::fabs(*tof_mm_ - *tof_ref_) >= cfg_.tof_leave_mm) { stop_low_breath(); return; }
  }
  // 대상이 보이는 채로 크게 움직이면 숨이 멈춘 것이 아니다.
  if (presence && movement_valid && move > cfg_.move_reject) stop_low_breath();
}

std::optional<Json> RadarBridge::step(double now) {
  const auto fresh = [&](double at) { return at >= 0 && now >= at && now - at <= cfg_.stale_s; };
  const bool presence_valid = fresh(presence_rx_) && presence_.has_value();
  const bool presence = presence_valid && *presence_;
  const auto dist = fresh(distance_rx_) ? distance_ : std::nullopt;
  const double move = movement(now);
  const auto raw_b = fresh(breath_rx_) ? breath_ : std::nullopt;
  const bool movement_valid = fresh(distance_rx_) && dist_hist_.size() >= 2;
  const bool current_valid = movement_valid && breath_is_trustworthy(raw_b, dist, presence, move);
  if (breath_rx_ != used_breath_rx_) {
    used_breath_rx_ = breath_rx_;
    if (current_valid) {
      br_hist_.emplace_back(breath_rx_, *raw_b);
      last_valid_ = breath_rx_;
    }
  }
  update_low_breath(raw_b, presence, dist, movement_valid, move, now);
  const bool apnea_valid = fresh(breath_rx_);  // 호흡 원본이 들어오고 있어야 판정할 수 있다
  const std::optional<double> low_s = low_since_ ? std::optional<double>(now - *low_since_) : std::nullopt;
  const bool apnea = apnea_valid && low_s && *low_s >= cfg_.apnea_hold_s;
  const auto smooth = smoothed_breath(now);
  // 방금 값이 게이트에서 떨어져도 창에 유효값이 남아 있는 동안은 그 중앙값을 유지한다(화면 깜빡임 방지).
  // 호흡이 약하게 잡히는 중(low_s)에는 유지값 대신 경과 시간을 보여 주도록 breath_valid를 내린다.
  const bool breath_valid = smooth.has_value() && !low_s;
  const double out_b = breath_valid ? *smooth : 0.0;

  Json msg = topics::radar_msg(py_round(out_b, 1), py_round(move, 2), presence, now);
  msg["apnea"] = apnea;
  msg["motion"] = move > 0.5;
  msg["source"] = "mr60bha2";
  msg["heart_rate"] = opt_json(fresh(heart_rx_) ? heart_ : std::nullopt);
  msg["distance"] = opt_json(dist);
  msg["raw_breath"] = opt_json(raw_b);  // 게이팅 전 원본(디버깅·비교용)
  msg["valid"] = presence_valid;
  msg["presence_valid"] = presence_valid;
  msg["movement_valid"] = movement_valid;
  msg["apnea_valid"] = apnea_valid;
  msg["breath_valid"] = breath_valid;
  msg["low_breath_s"] = opt_json(low_s);           // 원본이 apnea_low_br 이하로 이어진 시간(아니면 null)
  msg["apnea_hold_s"] = cfg_.apnea_hold_s;         // 화면이 단계를 나누는 기준
  msg["apnea_low_br"] = cfg_.apnea_low_br;
  msg["sample_id"] = breath_seq_;
  msg["breath_age_s"] = breath_rx_ < 0 ? Json(nullptr) : Json(now - breath_rx_);
  msg["distance_age_s"] = distance_rx_ < 0 ? Json(nullptr) : Json(now - distance_rx_);
  msg["presence_age_s"] = presence_rx_ < 0 ? Json(nullptr) : Json(now - presence_rx_);
  // 낮은 호흡값 하나로 바로 경보하지 않는다. 경보는 무호흡(지속 시간 판정)일 때만이다.
  msg["assessment"] = Json{{"breath_abnormal", apnea},
    {"borderline", breath_valid && ((out_b >= 8 && out_b <= 15) || out_b > 55)},
    {"movement_high", presence && movement_valid && move > 0.6},
    {"level", apnea ? "alert" : "normal"}};
  return msg;
}

}  // namespace nuni
