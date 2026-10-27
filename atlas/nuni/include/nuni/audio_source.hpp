// 마이크·WAV 입력 — 최근 소리를 RingBuffer에 계속 채운다.
//
// atlas 오디오 서버는 PulseAudio(시스템 모드)다. 기기에는 libpulse만 있고 libpulse-simple은 없어서
// libpulse 기본 API(threaded mainloop)로 녹음 스트림을 직접 연다.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "nuni/cry_detector.hpp"

namespace nuni {

class AudioSource {
 public:
  virtual ~AudioSource() = default;
  virtual bool start() = 0;
  virtual void stop() = 0;
  virtual bool ok() const = 0;  // 지금 소리가 들어오고 있는가
  virtual const char* name() const = 0;
};

// PulseAudio 녹음. server: 비우면 기본(환경변수 PULSE_SERVER 또는 시스템 소켓),
// source: 비우면 기본 입력 장치. 샘플레이트는 buf.sr()로 받는다(float32 모노).
std::unique_ptr<AudioSource> make_pulse_source(RingBuffer& buf, std::string server, std::string source);

// WAV 파일을 실시간 속도로 반복 재생하며 버퍼에 넣는다(마이크 없이 시연·시험용).
// 파일 샘플레이트가 buf.sr()과 다르면 resample_linear로 맞춘다.
std::unique_ptr<AudioSource> make_wav_source(RingBuffer& buf, const std::string& path, bool loop = true);

// WAV 읽기(PCM 16/24/32비트 정수, float32; 여러 채널이면 첫 채널). 실패하면 false.
bool read_wav(const std::string& path, std::vector<float>& out, int& sr);

}  // namespace nuni
