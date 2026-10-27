// YAMNet 추론 — ECO.AI 통합 API(ecoai-inference)로 돌린다.
//
// atlas Pi 5(26.06.0-246) 이미지에는 추론 플러그인(/usr/lib/ecoai/strategy_cfg: TfLite·Onnx·ExecuTorch)이 들어 있어
// 같은 .tflite가 그대로 돈다(atlas/yamnet_test/yamnet_ecoai_test.cpp, 2026-10-19: TFLite 직접과 출력 동일, cpu 8.8ms,
// gpu 17.5ms). Pi 4 이미지에는 플러그인이 없어 init이 실패하므로, 그때는 yamnet_tflite.hpp(TFLite 직접)로 넘어간다.
#pragma once

#include <memory>
#include <string>

#include "nuni/cry_detector.hpp"

namespace nuni {

// 플러그인이 없거나 모델을 읽지 못하면 nullptr. backend: "cpu"(기본) 또는 "gpu".
std::unique_ptr<AudioModel> make_yamnet_ecoai(const std::string& model_path, int threads = 1,
                                              const std::string& backend = "cpu");

}  // namespace nuni
