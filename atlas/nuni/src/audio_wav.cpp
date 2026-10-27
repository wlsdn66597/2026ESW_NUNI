#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <cstring>
#include <fstream>
#include <iostream>

#include "nuni/audio_source.hpp"

namespace nuni {

namespace {

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

class WavSource : public AudioSource {
 public:
  WavSource(RingBuffer& buf, std::string path, bool loop) : buf_(buf), path_(std::move(path)), loop_(loop) {}
  ~WavSource() override { stop(); }

  bool start() override {
    int sr = 0;
    if (!read_wav(path_, data_, sr)) return false;
    if (sr != buf_.sr()) data_ = resample_linear(data_, sr, buf_.sr());
    running_ = true;
    th_ = std::thread([this] { run(); });
    std::cerr << "[audio] WAV 입력 " << path_ << " (" << data_.size() / double(buf_.sr()) << "초"
              << (loop_ ? ", 반복" : "") << ")\n";
    return true;
  }

  void stop() override {
    running_ = false;
    if (th_.joinable()) th_.join();
  }

  bool ok() const override { return running_; }
  const char* name() const override { return "wav"; }

 private:
  void run() {
    const size_t chunk = static_cast<size_t>(buf_.sr() / 20);  // 50ms씩
    size_t pos = 0;
    auto next = std::chrono::steady_clock::now();
    while (running_) {
      if (pos >= data_.size()) {
        if (!loop_) break;
        pos = 0;
      }
      const size_t n = std::min(chunk, data_.size() - pos);
      buf_.push(data_.data() + pos, n);
      pos += n;
      next += std::chrono::milliseconds(50);
      std::this_thread::sleep_until(next);
    }
    running_ = false;
  }

  RingBuffer& buf_;
  std::string path_;
  bool loop_;
  std::vector<float> data_;
  std::atomic<bool> running_{false};
  std::thread th_;
};

}  // namespace

bool read_wav(const std::string& path, std::vector<float>& out, int& sr) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::cerr << "[audio] WAV를 열 수 없음: " << path << "\n";
    return false;
  }
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) {
    std::cerr << "[audio] WAV 형식이 아님: " << path << "\n";
    return false;
  }
  uint16_t fmt = 0, ch = 0, bits = 0;
  const uint8_t* pcm = nullptr;
  size_t pcm_len = 0;
  for (size_t off = 12; off + 8 <= d.size();) {
    const uint32_t sz = le32(d.data() + off + 4);
    const uint8_t* body = d.data() + off + 8;
    const size_t avail = std::min<size_t>(sz, d.size() - off - 8);
    if (!std::memcmp(d.data() + off, "fmt ", 4) && avail >= 16) {
      fmt = le16(body);
      ch = le16(body + 2);
      sr = static_cast<int>(le32(body + 4));
      bits = le16(body + 14);
      if (fmt == 0xFFFE && avail >= 26) fmt = le16(body + 24);  // WAVE_FORMAT_EXTENSIBLE의 하위 형식
    } else if (!std::memcmp(d.data() + off, "data", 4)) {
      pcm = body;
      pcm_len = avail;
    }
    off += 8 + sz + (sz & 1);
  }
  if (!pcm || ch == 0 || sr <= 0) {
    std::cerr << "[audio] WAV fmt/data 청크가 없음: " << path << "\n";
    return false;
  }
  const size_t bps = bits / 8, frame = bps * ch;
  if (frame == 0) return false;
  const size_t n = pcm_len / frame;
  out.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* p = pcm + i * frame;  // 첫 채널
    if (fmt == 3 && bits == 32) {
      float v;
      std::memcpy(&v, p, 4);
      out[i] = v;
    } else if (fmt == 1 && bits == 16) {
      out[i] = static_cast<int16_t>(le16(p)) / 32768.0f;
    } else if (fmt == 1 && bits == 24) {
      int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
      if (v & 0x800000) v |= ~0xFFFFFF;
      out[i] = static_cast<float>(v / 8388608.0);
    } else if (fmt == 1 && bits == 32) {
      out[i] = static_cast<float>(static_cast<int32_t>(le32(p)) / 2147483648.0);
    } else {
      std::cerr << "[audio] 지원하지 않는 WAV 형식(format " << fmt << ", " << bits << "비트)\n";
      return false;
    }
  }
  return true;
}

std::unique_ptr<AudioSource> make_wav_source(RingBuffer& buf, const std::string& path, bool loop) {
  return std::make_unique<WavSource>(buf, path, loop);
}

}  // namespace nuni
