#include "nuni/yamnet_tflite.hpp"

#include <cstring>
#include <iostream>

#include "tensorflow/lite/delegates/xnnpack/xnnpack_delegate.h"
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"

namespace nuni {

namespace {

class YamnetTflite : public AudioModel {
 public:
  bool init(const std::string& path, int threads) {
    model_ = tflite::FlatBufferModel::BuildFromFile(path.c_str());
    if (!model_) {
      std::cerr << "[yamnet] 모델 읽기 실패: " << path << "\n";
      return false;
    }
    tflite::ops::builtin::BuiltinOpResolver resolver;
    if (tflite::InterpreterBuilder(*model_, resolver)(&interp_) != kTfLiteOk || !interp_) {
      std::cerr << "[yamnet] 인터프리터 생성 실패\n";
      return false;
    }
    TfLiteXNNPackDelegateOptions opts = TfLiteXNNPackDelegateOptionsDefault();
    opts.num_threads = threads;
    xnn_ = DelegatePtr(TfLiteXNNPackDelegateCreate(&opts), TfLiteXNNPackDelegateDelete);
    if (interp_->ModifyGraphWithDelegate(xnn_.get()) != kTfLiteOk)
      std::cerr << "[yamnet] XNNPACK 위임 실패 — 기본 CPU로 진행\n";
    interp_->SetNumThreads(threads);
    if (interp_->AllocateTensors() != kTfLiteOk) {
      std::cerr << "[yamnet] AllocateTensors 실패\n";
      return false;
    }
    const TfLiteTensor* in = interp_->input_tensor(0);
    if (in->type != kTfLiteFloat32 || in->bytes / sizeof(float) != static_cast<size_t>(kYamnetWindow)) {
      std::cerr << "[yamnet] 입력이 float32 " << kYamnetWindow << "개가 아님: " << in->bytes / sizeof(float) << "\n";
      return false;
    }
    // 출력 순서는 모델마다 다르므로 원소 수로 찾는다(점수 521, 임베딩 1024)
    for (size_t o = 0; o < interp_->outputs().size(); ++o) {
      const size_t n = interp_->output_tensor(o)->bytes / sizeof(float);
      if (n == static_cast<size_t>(kYamnetClasses)) score_out_ = static_cast<int>(o);
      if (n == static_cast<size_t>(kYamnetEmbedding)) emb_out_ = static_cast<int>(o);
    }
    if (score_out_ < 0 && emb_out_ < 0) {
      std::cerr << "[yamnet] 점수(521)·임베딩(1024) 출력을 찾지 못함\n";
      return false;
    }
    if (score_out_ < 0) std::cerr << "[yamnet] 점수(521) 출력이 없는 모델 — 울음 판정에는 쓸 수 없음\n";
    std::cerr << "[yamnet] 모델 " << path << " (임베딩 " << (emb_out_ >= 0 ? "있음" : "없음") << ", 스레드 " << threads
              << ")\n";
    return true;
  }

  bool run(const float* x, std::vector<float>& scores, std::vector<float>& embedding) override {
    std::memcpy(interp_->typed_input_tensor<float>(0), x, kYamnetWindow * sizeof(float));
    if (interp_->Invoke() != kTfLiteOk) return false;
    if (score_out_ >= 0) {
      const float* s = interp_->typed_output_tensor<float>(score_out_);
      scores.assign(s, s + kYamnetClasses);
    } else {
      scores.clear();
    }
    if (emb_out_ >= 0) {
      const float* e = interp_->typed_output_tensor<float>(emb_out_);
      embedding.assign(e, e + kYamnetEmbedding);
    } else {
      embedding.clear();
    }
    return true;
  }

 private:
  using DelegatePtr = std::unique_ptr<TfLiteDelegate, decltype(&TfLiteXNNPackDelegateDelete)>;
  // 해제 순서가 중요하다: 인터프리터 → 위임 → 모델 (선언의 역순으로 해제된다)
  std::unique_ptr<tflite::FlatBufferModel> model_;
  DelegatePtr xnn_{nullptr, TfLiteXNNPackDelegateDelete};
  std::unique_ptr<tflite::Interpreter> interp_;
  int score_out_ = -1, emb_out_ = -1;
};

}  // namespace

std::unique_ptr<AudioModel> make_yamnet_tflite(const std::string& model_path, int threads) {
  auto m = std::make_unique<YamnetTflite>();
  if (!m->init(model_path, threads)) return nullptr;
  return m;
}

}  // namespace nuni
