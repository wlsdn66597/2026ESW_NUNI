// YAMNet 추론 — TensorFlow Lite 런타임 직접 사용(XNNPACK, CPU).
//
// atlas Pi 4에서 확인된 방식이다(atlas/yamnet_test/yamnet_tflite_test.cpp, 2026-10-19:
// 노트북 TFLite와 출력 동일, 0.975초 창 1개당 49ms(스레드 1) / 24ms(스레드 4)).
// Pi 4 이미지에는 ECO.AI 추론 플러그인이 없어 이 경로를 쓴다. Pi 5에서는 yamnet_ecoai.hpp(ECO.AI)가 먼저 쓰인다.
//
// 점수만 내는 TF Hub yamnet.tflite(출력 521)와 점수+임베딩 yamnet_emb.tflite(출력 521 + 1024)를 모두 받는다.
#pragma once

#include <memory>
#include <string>

#include "nuni/cry_detector.hpp"

namespace nuni {

// 모델을 읽지 못하면 nullptr. threads: XNNPACK 스레드 수.
std::unique_ptr<AudioModel> make_yamnet_tflite(const std::string& model_path, int threads = 1);

}  // namespace nuni
