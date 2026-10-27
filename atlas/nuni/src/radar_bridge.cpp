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
    dist_hist_.clear(); br_hist_.clear(); last_valid_.reset(); no_valid_since_.reset();
    return;
  } else if (topic == p + "/num_targets") {
    // ESPHome presence is published on change; target count is periodic.
    // It refreshes presence only, never the age of a breath measurement.
    const auto count=parse_py_float(raw);
    if(!count || !std::isfinite(*count) || *count<0 || std::floor(*count)!=*count) return;
    presence_=*count>0; presence_rx_=now;
    if(!*presence_) { br_hist_.clear(); last_valid_.reset(); no_valid_since_.reset(); }
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
    if (slot == &distance_ && !std::isnan(*val)) dist_hist_.emplace_back(now, *val);
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

bool RadarBridge::breath_is_trustworthy(std::optional<double> bpm, std::optional<double> dist, bool presence,
                                        double move) const {
  if (!presence || !bpm || std::isnan(*bpm)) return false;
  if (!(cfg_.br_min <= *bpm && *bpm <= cfg_.br_max)) return false;  // 0·튄 값 배제
  if (!dist || std::isnan(*dist) || !(cfg_.dist_min_cm <= *dist && *dist <= cfg_.dist_max_cm))
    return false;                                                     // 유효 거리 밖이면 호흡값 무의미
  return move <= cfg_.move_reject;                                    // 크게 움직이면 호흡 신호가 묻힘
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

bool RadarBridge::derive_apnea(bool presence, std::optional<double> dist, double move, double now) {
  // 무호흡(잘 재던 호흡이 갑자기 사라짐)과 미측정(애초에 호흡이 안 잡힘)을 구분한다.
  const bool measurable = presence && dist && !std::isnan(*dist) && cfg_.dist_min_cm <= *dist &&
                          *dist <= cfg_.dist_max_cm && move <= cfg_.move_reject;
  const bool had_baseline = last_valid_ && (now - *last_valid_) <= cfg_.apnea_require_prior_s;
  if (!(measurable && had_baseline)) {
    no_valid_since_.reset();  // 측정 불가 or 기준 호흡 없음 → 판단 보류
    return false;
  }
  if (!br_hist_.empty() && now - br_hist_.back().first <= 1.5) {
    no_valid_since_.reset();  // 방금 유효 호흡이 잡힘
    return false;
  }
  if (!no_valid_since_) no_valid_since_ = now;
  return (now - *no_valid_since_) >= cfg_.apnea_hold_s;
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
  const bool apnea_valid = presence_valid && presence && fresh(breath_rx_) && movement_valid && dist.has_value();
  const bool apnea = apnea_valid && derive_apnea(presence, dist, move, now);
  if (!apnea_valid) no_valid_since_.reset();
  const auto smooth = smoothed_breath(now);
  // 유효값이 잠깐 끊겨도 창이 유지되는 동안은 마지막 값을 유지한다(화면 깜빡임 방지).
  const double out_b = !smooth || !current_valid ? 0.0 : (apnea ? 0.0 : *smooth);

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
  msg["breath_valid"] = current_valid && smooth.has_value();
  msg["sample_id"] = breath_seq_;
  msg["breath_age_s"] = breath_rx_ < 0 ? Json(nullptr) : Json(now - breath_rx_);
  msg["distance_age_s"] = distance_rx_ < 0 ? Json(nullptr) : Json(now - distance_rx_);
  msg["presence_age_s"] = presence_rx_ < 0 ? Json(nullptr) : Json(now - presence_rx_);
  msg["assessment"] = Json{{"breath_abnormal", apnea || (current_valid && out_b > 0 && out_b < 8)},
    {"borderline", current_valid && ((out_b >= 8 && out_b <= 15) || out_b > 55)},
    {"movement_high", presence && movement_valid && move > 0.6},
    {"level", apnea || (current_valid && out_b > 0 && out_b < 8) ? "alert" : "normal"}};
  return msg;
}

}  // namespace nuni
