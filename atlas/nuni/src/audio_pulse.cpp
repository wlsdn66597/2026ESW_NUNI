// PulseAudio 녹음 입력 — libpulse threaded mainloop 사용(atlas 기기에 libpulse-simple이 없어서).
#include <pulse/pulseaudio.h>

#include <chrono>
#include <iostream>

#include "nuni/audio_source.hpp"

namespace nuni {

namespace {

double mono_now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

class PulseSource : public AudioSource {
 public:
  PulseSource(RingBuffer& buf, std::string server, std::string source)
      : buf_(buf), server_(std::move(server)), source_(std::move(source)) {}
  ~PulseSource() override { stop(); }

  bool start() override {
    stop();
    failed_ = false;
    ml_ = pa_threaded_mainloop_new();
    if (!ml_) return fail("mainloop 생성 실패");
    ctx_ = pa_context_new(pa_threaded_mainloop_get_api(ml_), "nuni-cry");
    if (!ctx_) return fail("context 생성 실패");
    pa_context_set_state_callback(ctx_, &PulseSource::ctx_cb, this);
    if (pa_context_connect(ctx_, server_.empty() ? nullptr : server_.c_str(), PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0)
      return fail(pa_strerror(pa_context_errno(ctx_)));

    pa_threaded_mainloop_lock(ml_);
    if (pa_threaded_mainloop_start(ml_) < 0) {
      pa_threaded_mainloop_unlock(ml_);
      return fail("mainloop 시작 실패");
    }
    started_ = true;
    while (true) {  // 서버 접속 대기
      const auto s = pa_context_get_state(ctx_);
      if (s == PA_CONTEXT_READY) break;
      if (!PA_CONTEXT_IS_GOOD(s)) {
        const std::string err = pa_strerror(pa_context_errno(ctx_));
        pa_threaded_mainloop_unlock(ml_);
        return fail(("서버 접속 실패: " + err).c_str());
      }
      pa_threaded_mainloop_wait(ml_);
    }

    pa_sample_spec ss{};
    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.rate = static_cast<uint32_t>(buf_.sr());
    ss.channels = 1;
    st_ = pa_stream_new(ctx_, "nuni cry detect", &ss, nullptr);
    if (!st_) {
      pa_threaded_mainloop_unlock(ml_);
      return fail("stream 생성 실패");
    }
    pa_stream_set_state_callback(st_, &PulseSource::st_cb, this);
    pa_stream_set_read_callback(st_, &PulseSource::read_cb, this);
    pa_buffer_attr attr{};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength = attr.prebuf = attr.minreq = static_cast<uint32_t>(-1);
    attr.fragsize = static_cast<uint32_t>(pa_usec_to_bytes(100000, &ss));  // 100ms마다 받기
    if (pa_stream_connect_record(st_, source_.empty() ? nullptr : source_.c_str(), &attr, PA_STREAM_ADJUST_LATENCY) <
        0) {
      const std::string err = pa_strerror(pa_context_errno(ctx_));
      pa_threaded_mainloop_unlock(ml_);
      return fail(("녹음 연결 실패: " + err).c_str());
    }
    while (true) {
      const auto s = pa_stream_get_state(st_);
      if (s == PA_STREAM_READY) break;
      if (!PA_STREAM_IS_GOOD(s)) {
        const std::string err = pa_strerror(pa_context_errno(ctx_));
        pa_threaded_mainloop_unlock(ml_);
        return fail(("녹음 스트림 실패: " + err).c_str());
      }
      pa_threaded_mainloop_wait(ml_);
    }
    const char* dev = pa_stream_get_device_name(st_);
    pa_threaded_mainloop_unlock(ml_);
    last_data_ = mono_now();
    std::cerr << "[audio] PulseAudio 녹음 시작: 장치 " << (dev ? dev : "?") << ", " << buf_.sr() << "Hz 모노\n";
    return true;
  }

  void stop() override {
    if (ml_) {
      if (started_) pa_threaded_mainloop_lock(ml_);
      if (st_) {
        pa_stream_disconnect(st_);
        pa_stream_unref(st_);
        st_ = nullptr;
      }
      if (ctx_) {
        pa_context_disconnect(ctx_);
        pa_context_unref(ctx_);
        ctx_ = nullptr;
      }
      if (started_) {
        pa_threaded_mainloop_unlock(ml_);
        pa_threaded_mainloop_stop(ml_);
      }
      pa_threaded_mainloop_free(ml_);
      ml_ = nullptr;
      started_ = false;
    }
  }

  // 실패하지 않았고, 3초 안에 소리가 들어왔다
  bool ok() const override { return ml_ && !failed_ && mono_now() - last_data_ < 3.0; }
  const char* name() const override { return "pulse"; }

 private:
  bool fail(const char* why) {
    std::cerr << "[audio] PulseAudio: " << why << "\n";
    failed_ = true;
    stop();
    return false;
  }

  static void ctx_cb(pa_context* c, void* ud) {
    auto* self = static_cast<PulseSource*>(ud);
    if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(c))) self->failed_ = true;
    pa_threaded_mainloop_signal(self->ml_, 0);
  }

  static void st_cb(pa_stream* s, void* ud) {
    auto* self = static_cast<PulseSource*>(ud);
    if (!PA_STREAM_IS_GOOD(pa_stream_get_state(s))) self->failed_ = true;
    pa_threaded_mainloop_signal(self->ml_, 0);
  }

  static void read_cb(pa_stream* s, size_t, void* ud) {
    auto* self = static_cast<PulseSource*>(ud);
    while (pa_stream_readable_size(s) > 0) {
      const void* data = nullptr;
      size_t len = 0;
      if (pa_stream_peek(s, &data, &len) < 0) {
        self->failed_ = true;
        return;
      }
      if (len == 0) break;
      if (data) {
        self->buf_.push(static_cast<const float*>(data), len / sizeof(float));
      } else {  // 구멍(hole): 그 길이만큼 무음
        std::vector<float> z(len / sizeof(float), 0.f);
        self->buf_.push(z.data(), z.size());
      }
      pa_stream_drop(s);
      self->last_data_ = mono_now();
    }
  }

  RingBuffer& buf_;
  std::string server_, source_;
  pa_threaded_mainloop* ml_ = nullptr;
  pa_context* ctx_ = nullptr;
  pa_stream* st_ = nullptr;
  bool started_ = false;
  std::atomic<bool> failed_{false};
  std::atomic<double> last_data_{0};
};

}  // namespace

std::unique_ptr<AudioSource> make_pulse_source(RingBuffer& buf, std::string server, std::string source) {
  return std::make_unique<PulseSource>(buf, std::move(server), std::move(source));
}

}  // namespace nuni
