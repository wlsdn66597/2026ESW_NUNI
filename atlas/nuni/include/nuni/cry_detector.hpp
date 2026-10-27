// 울음 감지 — cry_model/yamnet_cry.py + cry_classifier.py(실물 경로) 이식.
//
// 마이크 최근 1.5초(마이크 고유 샘플레이트) → 16kHz 선형 리샘플 → YAMNet 내장 클래스
// "Crying, sobbing"(19)·"Baby cry, infant cry"(20) 점수 중 큰 값 → 임계값(0.3)을 넘으면 울음.
// 울음 '이유'는 분류하지 않는다(cls는 항상 "none").
//
// 추론 엔진은 AudioModel 인터페이스로 분리했다. atlas에서는 TFLite 직접 사용(yamnet_tflite.cpp),
// ECO.AI 추론 애드온이 생기면 같은 인터페이스로 바꿔 끼우면 된다.
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "nuni/json_util.hpp"

namespace nuni {

inline constexpr int kYamnetWindow = 15600;  // YAMNet 고정 입력 길이 (0.975초 @ 16kHz)
inline constexpr int kYamnetSr = 16000;
inline constexpr int kYamnetClasses = 521;
inline constexpr int kYamnetEmbedding = 1024;

// numpy 선형보간 리샘플(np.linspace + np.interp)과 같은 계산.
std::vector<float> resample_linear(const std::vector<float>& x, int orig_sr, int target_sr = kYamnetSr);

// YAMNet 같은 소리 분류 모델(입력 15600 float → 점수 521, 선택적으로 임베딩 1024).
class AudioModel {
 public:
  virtual ~AudioModel() = default;
  // 성공하면 true. scores는 521개, embedding은 모델이 주면 1024개(없으면 비워 둔다).
  virtual bool run(const float* window15600, std::vector<float>& scores, std::vector<float>& embedding) = 0;
};

struct CryResult {
  double cry_score = 0.0;          // max(점수[19], 점수[20])
  std::string label;               // 둘 중 큰 쪽 이름
  std::vector<float> embedding;    // 모델이 임베딩을 주면 채움(울음 힌트 2단계용)
};

class CryDetector {
 public:
  // class_map_csv가 주어지면 이름으로 클래스 번호를 찾고, 없으면 19·20을 쓴다.
  explicit CryDetector(std::unique_ptr<AudioModel> model, const std::string& class_map_csv = "");

  // wav16k: 16kHz 모노 -1~1. 15600보다 짧으면 뒤를 0으로 채우고, 길면 마지막 15600개를 쓴다.
  std::optional<CryResult> predict(const std::vector<float>& wav16k);

  // 마이크 원본(native_sr) 창 → 리샘플 → 판정 → audio/cry 메시지.
  std::optional<Json> classify(const std::vector<float>& raw, int native_sr, double threshold, double ts,
                               CryResult* detail = nullptr);

 private:
  std::unique_ptr<AudioModel> model_;
  std::vector<std::pair<std::string, int>> cry_idx_;  // CSV 순서 유지(동점이면 앞쪽 이름 — Python max와 같음)
  std::vector<float> scores_, emb_;
};

// 최근 N초를 유지하는 링버퍼(cry_model/infer.py의 Mic 버퍼). 여러 스레드에서 써도 안전하다.
class RingBuffer {
 public:
  RingBuffer(int sr, double seconds);
  void push(const float* x, size_t n);
  std::vector<float> last(double seconds) const;  // 처음엔 0으로 채워진 상태(Python과 같음)
  int sr() const { return sr_; }

 private:
  int sr_;
  std::vector<float> buf_;
  size_t head_ = 0;  // 다음에 쓸 위치
  mutable std::mutex mu_;
};

}  // namespace nuni
