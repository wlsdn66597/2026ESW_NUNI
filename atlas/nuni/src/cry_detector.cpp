#include "nuni/cry_detector.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

#include "nuni/topics.hpp"

namespace nuni {

namespace {

// np.linspace(0, 1, n, endpoint=False): y[i] = i * (1.0 / n)
std::vector<double> linspace01(size_t n) {
  std::vector<double> t(n);
  const double step = 1.0 / static_cast<double>(n);
  for (size_t i = 0; i < n; ++i) t[i] = static_cast<double>(i) * step;
  return t;
}

// CSV 한 줄(따옴표·"" 처리)을 칸으로 나눈다.
std::vector<std::string> split_csv(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool q = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (q) {
      if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
      else if (c == '"') q = false;
      else cur += c;
    } else if (c == '"') {
      q = true;
    } else if (c == ',') {
      out.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

}  // namespace

std::vector<float> resample_linear(const std::vector<float>& x, int orig_sr, int target_sr) {
  if (orig_sr == target_sr || x.empty()) return x;
  const size_t n_in = x.size();
  const auto n_out = static_cast<size_t>(
      std::nearbyint(static_cast<double>(n_in) * target_sr / static_cast<double>(orig_sr)));  // round(): 짝수 반올림
  const std::vector<double> t_in = linspace01(n_in), t_out = linspace01(n_out);
  std::vector<float> y(n_out);
  // np.interp(t_out, t_in, x)와 같은 분기·계산 순서
  for (size_t i = 0; i < n_out; ++i) {
    const double xv = t_out[i];
    double r;
    if (xv > t_in[n_in - 1]) {
      r = x[n_in - 1];
    } else if (xv < t_in[0]) {
      r = x[0];
    } else {
      const size_t j = static_cast<size_t>(std::upper_bound(t_in.begin(), t_in.end(), xv) - t_in.begin()) - 1;
      if (j == n_in - 1 || t_in[j] == xv) {
        r = x[j];
      } else {
        const double y0 = x[j], y1 = x[j + 1];
        const double slope = (y1 - y0) / (t_in[j + 1] - t_in[j]);
        r = slope * (xv - t_in[j]) + y0;
        if (std::isnan(r)) {
          r = slope * (xv - t_in[j + 1]) + y1;
          if (std::isnan(r) && y0 == y1) r = y0;
        }
      }
    }
    y[i] = static_cast<float>(r);
  }
  return y;
}

CryDetector::CryDetector(std::unique_ptr<AudioModel> model, const std::string& class_map_csv)
    : model_(std::move(model)) {
  static const char* kNames[] = {"Baby cry, infant cry", "Crying, sobbing"};
  if (!class_map_csv.empty()) {
    std::ifstream f(class_map_csv);
    std::string line;
    bool header = true;
    while (f && std::getline(f, line)) {
      if (header) { header = false; continue; }  // index,mid,display_name
      auto cols = split_csv(line);
      if (cols.size() < 3) continue;
      for (const char* n : kNames)
        if (cols[2] == n) cry_idx_.emplace_back(cols[2], std::stoi(cols[0]));
    }
    if (cry_idx_.empty()) std::cerr << "[cry] 클래스 맵에서 울음 클래스를 찾지 못해 기본 번호(19·20)를 씁니다\n";
  }
  if (cry_idx_.empty()) cry_idx_ = {{"Crying, sobbing", 19}, {"Baby cry, infant cry", 20}};
}

std::optional<CryResult> CryDetector::predict(const std::vector<float>& wav16k) {
  std::vector<float> x(kYamnetWindow, 0.f);
  if (wav16k.size() < static_cast<size_t>(kYamnetWindow)) std::copy(wav16k.begin(), wav16k.end(), x.begin());
  else std::copy(wav16k.end() - kYamnetWindow, wav16k.end(), x.begin());

  if (!model_ || !model_->run(x.data(), scores_, emb_) || scores_.size() < static_cast<size_t>(kYamnetClasses))
    return std::nullopt;
  CryResult r;
  bool first = true;
  for (const auto& [name, idx] : cry_idx_) {
    const double s = scores_[idx];
    if (first || s > r.cry_score) {
      r.cry_score = s;
      r.label = name;
      first = false;
    }
  }
  r.embedding = emb_;
  return r;
}

std::optional<Json> CryDetector::classify(const std::vector<float>& raw, int native_sr, double threshold, double ts,
                                          CryResult* detail) {
  const auto x16 = resample_linear(raw, native_sr);
  auto r = predict(x16);
  if (!r) return std::nullopt;
  if (detail) *detail = *r;
  return topics::cry_msg(r->cry_score > threshold, "none", py_round(r->cry_score, 2), ts);
}

RingBuffer::RingBuffer(int sr, double seconds) : sr_(sr), buf_(static_cast<size_t>(sr * seconds), 0.f) {}

void RingBuffer::push(const float* x, size_t n) {
  std::lock_guard<std::mutex> lk(mu_);
  const size_t cap = buf_.size();
  if (cap == 0) return;
  if (n > cap) {  // 버퍼보다 길면 마지막 cap개만
    x += n - cap;
    n = cap;
  }
  for (size_t i = 0; i < n; ++i) {
    buf_[head_] = x[i];
    head_ = (head_ + 1) % cap;
  }
}

std::vector<float> RingBuffer::last(double seconds) const {
  std::lock_guard<std::mutex> lk(mu_);
  const size_t cap = buf_.size();
  const size_t n = std::min(cap, static_cast<size_t>(sr_ * seconds));
  std::vector<float> out(n);
  size_t start = (head_ + cap - n) % (cap ? cap : 1);
  for (size_t i = 0; i < n; ++i) out[i] = buf_[(start + i) % cap];
  return out;
}

}  // namespace nuni
