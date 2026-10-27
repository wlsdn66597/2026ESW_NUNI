#include "nuni/yamnet_ecoai.hpp"

#include <cstring>
#include <iostream>
#include <unordered_map>

#include <ecoai-inference/EcoTypes.hpp>
#include <ecoai-inference/InferenceStrategy/InferenceStrategy.hpp>
#include <ecoai-inference/InferenceStrategy/InferenceStrategyBuilder.hpp>

namespace nuni {

namespace {

namespace inf = ecoai::inference;

class YamnetEcoai : public AudioModel {
 public:
  bool init(const std::string& path, int threads, const std::string& backend) {
    std::unordered_map<std::string, std::string> options;
    options[backend] = "";
    options[backend + ".intra_op_num_threads"] = std::to_string(threads);
    try {
      strategy_ = inf::InferenceStrategyBuilder(path).setOptions(options).build();
    } catch (const std::exception& e) {
      std::cerr << "[yamnet] ECO.AI strategy 생성 실패: " << e.what() << "\n";
      return false;
    }
    if (!strategy_ || !strategy_->initialize()) {
      std::cerr << "[yamnet] ECO.AI initialize 실패\n";
      return false;
    }
    auto inputs = strategy_->getInputTensorsInfo();
    if (inputs.empty() || inputs[0].dataType != inf::EcoTensorInfo::DataType::FLOAT32) {
      std::cerr << "[yamnet] ECO.AI 입력이 float32가 아님\n";
      return false;
    }
    in_info_ = inputs[0];
    x_.assign(kYamnetWindow, 0.f);
    // 출력 이름은 한 번 돌려 보고 원소 수로 찾는다(점수 521, 임베딩 1024)
    if (!execute()) {
      std::cerr << "[yamnet] ECO.AI 첫 실행 실패\n";
      return false;
    }
    for (auto& kv : out_) {
      const size_t n = kv.second.size / sizeof(float);
      if (n == static_cast<size_t>(kYamnetClasses)) score_name_ = kv.first;
      if (n == static_cast<size_t>(kYamnetEmbedding)) emb_name_ = kv.first;
    }
    if (score_name_.empty() && emb_name_.empty()) {
      std::cerr << "[yamnet] 점수(521)·임베딩(1024) 출력을 찾지 못함\n";
      return false;
    }
    if (score_name_.empty()) std::cerr << "[yamnet] 점수(521) 출력이 없는 모델 — 울음 판정에는 쓸 수 없음\n";
    std::cerr << "[yamnet] ECO.AI 모델 " << path << " (백엔드 " << backend << ", 임베딩 "
              << (emb_name_.empty() ? "없음" : "있음") << ", 스레드 " << threads << ")\n";
    return true;
  }

  bool run(const float* x, std::vector<float>& scores, std::vector<float>& embedding) override {
    std::memcpy(x_.data(), x, kYamnetWindow * sizeof(float));
    if (!execute()) return false;
    copy_out(score_name_, kYamnetClasses, scores);
    copy_out(emb_name_, kYamnetEmbedding, embedding);
    return true;
  }

 private:
  bool execute() {
    inf::EcoTensorMap in;
    inf::EcoTensorData td;
    td.info = in_info_;
    td.data = x_.data();
    td.size = x_.size() * sizeof(float);
    td.owns_data = false;
    in[in_info_.name] = td;
    return strategy_->execute(in, out_);
  }

  void copy_out(const std::string& name, int n, std::vector<float>& dst) {
    auto it = name.empty() ? out_.end() : out_.find(name);
    if (it == out_.end()) {
      dst.clear();
      return;
    }
    const float* p = static_cast<const float*>(it->second.data);
    dst.assign(p, p + n);
  }

  inf::StrategyPtr strategy_;
  inf::EcoTensorInfo in_info_;
  inf::EcoTensorMap out_;
  std::vector<float> x_;
  std::string score_name_, emb_name_;
};

}  // namespace

std::unique_ptr<AudioModel> make_yamnet_ecoai(const std::string& model_path, int threads, const std::string& backend) {
  auto m = std::make_unique<YamnetEcoai>();
  if (!m->init(model_path, threads, backend)) return nullptr;
  return m;
}

}  // namespace nuni
