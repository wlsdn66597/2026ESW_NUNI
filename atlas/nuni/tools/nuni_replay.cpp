// nuni_replay — Python 원본과 같은 입력을 넣어 같은 출력이 나오는지 비교하는 시험 도구.
//
// 표준입력으로 JSON 명령을 한 줄씩 받아, 결과를 한 줄씩 표준출력으로 낸다. 짝이 되는 Python 쪽은
// tests/py_replay.py이고, tests/run_parity.sh가 둘을 같은 시나리오로 돌려 줄 단위로 비교한다.
//
// 명령(op)
//   fusion_new {debounce_n, cooldown_s}       새 Fusion(가짜 시계)
//   fusion {t, topic, payload}                메시지 하나 → 발행된 메시지들 + 결과
//   fusion_update {t, radar, cry, env}        세 모달 한꺼번에 갱신
//   decide / single {radar, cry, env}         판단 규칙만
//   sleep {radar, cry}                        수면 상태
//   env {env, ss}                             환경 권고
//   radar_new {cfg}                           새 RadarBridge(설정 덮어쓰기)
//   esphome {t, topic, raw}                   ESPHome 평문 수신
//   radar_step {t}                            sensor/radar 메시지(없으면 null)
//   resample {x, sr, target}                  선형 리샘플
//   pyround {x, n}                            Python round
//   cry_wav {wav, model, classmap}            WAV를 1초마다 1.5초 창으로 판정(TFLite 빌드에서만)
//   model_wav {wav, model}                    같은 창으로 모델 출력(점수 상위 5개·임베딩 앞 8개·노름)만 비교
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include "nuni/cry_detector.hpp"
#include "nuni/env_control.hpp"
#include "nuni/fusion.hpp"
#include "nuni/json_util.hpp"
#include "nuni/radar_bridge.hpp"
#include "nuni/sleep_state.hpp"
#ifdef NUNI_WITH_TFLITE
#include "nuni/audio_source.hpp"
#include "nuni/yamnet_tflite.hpp"
#endif

using namespace nuni;

namespace {

Json decision_json(const Decision& d) { return Json{{"level", d.level}, {"reasons", d.reasons}}; }

}  // namespace

int main() {
  std::ios::sync_with_stdio(false);
  double fake_now = 0;
  Json pubs = Json::array();
  std::unique_ptr<Fusion> fusion;
  std::unique_ptr<RadarBridge> radar;
  auto out = [](const Json& j) { std::cout << dump(j) << "\n"; };

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    auto parsed = parse_payload(line);
    if (!parsed) {
      out(Json{{"error", "bad json"}});
      continue;
    }
    const Json& c = *parsed;
    const std::string op = c.value("op", "");
    if (c.contains("t")) fake_now = c["t"].get<double>();

    if (op == "fusion_new") {
      fusion = std::make_unique<Fusion>([&](const std::string& t, const Json& p) { pubs.push_back(Json{{"topic", t}, {"payload", p}}); },
                                        c.value("debounce_n", 2), c.value("cooldown_s", 10.0), [&] { return fake_now; });
      out(Json{{"ok", true}});
    } else if (op == "fusion" || op == "fusion_update") {
      pubs = Json::array();
      Fusion::Result r = op == "fusion" ? fusion->on_message(c.value("topic", ""), c.value("payload", Json()))
                                        : fusion->update(c.value("radar", Json()), c.value("cry", Json()), c.value("env", Json()));
      out(Json{{"level", r.level}, {"reasons", r.reasons}, {"emitted", r.alert_emitted}, {"pubs", pubs}});
    } else if (op == "decide") {
      out(decision_json(decide(c.value("radar", Json()), c.value("cry", Json()), c.value("env", Json()))));
    } else if (op == "single") {
      out(decision_json(single_sensor_decide(c.value("radar", Json()), c.value("cry", Json()), c.value("env", Json()))));
    } else if (op == "sleep") {
      auto r = sleep_state::classify(c.value("radar", Json()), c.value("cry", Json()));
      out(Json{{"state", r.state}, {"reason", r.reason}});
    } else if (op == "env") {
      Json acts = Json::array();
      for (const auto& a : env_control::recommend(c.value("env", Json()), c.value("ss", "")))
        acts.push_back(Json::array({a.first, a.second}));
      out(Json{{"actions", acts}});
    } else if (op == "radar_new") {
      RadarConfig cfg;
      const Json k = c.value("cfg", Json::object());
      auto num = [&](const char* key, double& dst) {
        if (k.contains(key)) dst = k[key].get<double>();
      };
      num("apnea_hold_s", cfg.apnea_hold_s);
      num("apnea_require_prior_s", cfg.apnea_require_prior_s);
      num("stale_s", cfg.stale_s);
      num("move_window_s", cfg.move_window_s);
      num("move_scale_cm", cfg.move_scale_cm);
      num("br_min", cfg.br_min);
      num("br_max", cfg.br_max);
      num("dist_min_cm", cfg.dist_min_cm);
      num("dist_max_cm", cfg.dist_max_cm);
      num("move_reject", cfg.move_reject);
      num("smooth_window_s", cfg.smooth_window_s);
      radar = std::make_unique<RadarBridge>(cfg);
      out(Json{{"ok", true}});
    } else if (op == "esphome") {
      radar->on_esphome(c.value("topic", ""), c.value("raw", ""), fake_now);
      out(Json{{"ok", true}});
    } else if (op == "radar_step") {
      auto m = radar->step(fake_now);
      out(Json{{"radar", m ? *m : Json()}});
    } else if (op == "resample") {
      auto x = c["x"].get<std::vector<float>>();
      auto y = resample_linear(x, c["sr"].get<int>(), c["target"].get<int>());
      out(Json{{"y", y}});
    } else if (op == "pyround") {
      out(Json{{"v", py_round(c["x"].get<double>(), c["n"].get<int>())}});
#ifdef NUNI_WITH_TFLITE
    } else if (op == "cry_wav") {
      std::vector<float> audio;
      int sr = 0;
      if (!read_wav(c.value("wav", ""), audio, sr)) {
        out(Json{{"error", "wav"}});
        continue;
      }
      auto model = make_yamnet_tflite(c.value("model", ""), 1);
      if (!model) {
        out(Json{{"error", "model"}});
        continue;
      }
      CryDetector det(std::move(model), c.value("classmap", ""));
      Json rows = Json::array();
      const size_t win = static_cast<size_t>(sr * 1.5);
      for (size_t end = sr; end <= audio.size(); end += sr) {  // 1초마다 최근 1.5초(앞은 0으로 채움 — 링버퍼 초기 상태)
        std::vector<float> w(win, 0.f);
        const size_t take = std::min(win, end);
        std::copy(audio.begin() + static_cast<long>(end - take), audio.begin() + static_cast<long>(end), w.end() - static_cast<long>(take));
        CryResult r;
        auto m = det.classify(w, sr, 0.3, 0.0, &r);
        rows.push_back(Json{{"score", r.cry_score}, {"label", r.label}, {"msg", m ? *m : Json()}});
      }
      out(Json{{"rows", rows}});
    } else if (op == "model_wav") {
      std::vector<float> audio;
      int sr = 0;
      auto model = make_yamnet_tflite(c.value("model", ""), 1);
      if (!read_wav(c.value("wav", ""), audio, sr) || !model) {
        out(Json{{"error", "wav/model"}});
        continue;
      }
      Json rows = Json::array();
      const size_t win = static_cast<size_t>(sr * 1.5);
      std::vector<float> scores, emb;
      for (size_t end = sr; end <= audio.size(); end += sr) {
        std::vector<float> w(win, 0.f);
        const size_t take = std::min(win, end);
        std::copy(audio.begin() + static_cast<long>(end - take), audio.begin() + static_cast<long>(end), w.end() - static_cast<long>(take));
        auto x = resample_linear(w, sr);
        std::vector<float> in(kYamnetWindow, 0.f);
        if (x.size() < in.size()) std::copy(x.begin(), x.end(), in.begin());
        else std::copy(x.end() - kYamnetWindow, x.end(), in.begin());
        model->run(in.data(), scores, emb);
        Json row;
        if (!scores.empty()) row["cry"] = std::max(scores[19], scores[20]);
        if (!emb.empty()) {
          double n2 = 0;
          for (float v : emb) n2 += double(v) * v;
          row["emb8"] = std::vector<float>(emb.begin(), emb.begin() + 8);
          row["norm"] = std::sqrt(n2);
        }
        rows.push_back(row);
      }
      out(Json{{"rows", rows}});
#endif
    } else {
      out(Json{{"error", "unknown op " + op}});
    }
  }
  return 0;
}
