// libpulse 없이 빌드할 때의 대체 구현 — 마이크는 쓸 수 없고, WAV 입력(--wav)만 된다.
#include <iostream>

#include "nuni/audio_source.hpp"

namespace nuni {

namespace {
class NoPulse : public AudioSource {
 public:
  bool start() override {
    std::cerr << "[audio] libpulse 없이 빌드됨 — 마이크를 쓸 수 없음(--wav로 파일 입력은 가능)\n";
    return false;
  }
  void stop() override {}
  bool ok() const override { return false; }
  const char* name() const override { return "none"; }
};
}  // namespace

std::unique_ptr<AudioSource> make_pulse_source(RingBuffer&, std::string, std::string) {
  return std::make_unique<NoPulse>();
}

}  // namespace nuni
