// cry_log_replay — CryLog를 사건 순서대로 재생해 결과를 JSON 줄로 낸다(tests/cry_log_parity.py가 Python CryPersonal과 비교).
//
// 사용법: cry_log_replay <기록 폴더> [cry_personal_ctx.json] < 사건.jsonl > 결과.jsonl
// 사건(한 줄에 하나):
//   {"op":"start","t":T}            울음 시작(CryPersonal.start)
//   {"op":"window"}                 울음 1초(CryPersonal.add_window, 임베딩·소리는 넣지 않는다)
//   {"op":"end","t":T}              울음 끝(CryPersonal.end) — 결과에 끝난 울음의 번호(index)
//   {"op":"feedback","index":K,"action":A}   K번째로 끝난 울음에 답(CryPersonal.feedback)
//   {"op":"not","index":K,"action":A}        '해 봤는데 안 그쳤어요'(CryPersonal.feedback_not)
//   {"op":"feed","t":T}             수유 기록(CryPersonal.log_feed)
//   {"op":"hint","t":T}             힌트(CryPersonal.hint(t, embs=[]))
//   {"op":"tick","t":T,"score":S}   1초 판정(울음 구간 나누기 시험용)
#include <iostream>
#include <string>
#include <vector>

#include "nuni/cry_log.hpp"

using namespace nuni;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "사용법: " << argv[0] << " <기록 폴더> [cry_personal_ctx.json] < 사건.jsonl\n";
    return 2;
  }
  CryLogConfig cfg;
  cfg.dir = argv[1];
  cfg.ctx_path = argc > 2 ? argv[2] : "";
  cfg.record = false;
  CryLog log(cfg);
  std::vector<std::string> ids;  // 끝난 울음 번호 → id
  const std::vector<float> none;
  std::string line;
  auto emit = [&](const char* op, Json out) {
    out["op"] = op;
    std::cout << out.dump() << "\n";
  };
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    const Json c = Json::parse(line, nullptr, false);
    if (c.is_discarded()) continue;
    const std::string op = c.value("op", "");
    const double t = c.value("t", 0.0);
    if (op == "start") {
      log.start(t);
    } else if (op == "window") {
      log.add_window(none, none, 1.0);
    } else if (op == "end") {
      auto ev = log.end(t);
      Json out = ev ? ev->data : Json::object();
      if (ev && ev->kind == "end") {
        out["index"] = ids.size();
        ids.push_back(ev->data.value("id", ""));
      }
      emit("end", out);
    } else if (op == "feedback" || op == "not") {
      const size_t k = c.value("index", 0);
      std::string err;
      const std::string id = k < ids.size() ? ids[k] : "";
      auto ev = op == "feedback" ? log.feedback(id, c.value("action", ""), t, &err)
                                 : log.feedback_not(id, c.value("action", ""), t, &err);
      emit(op.c_str(), ev ? ev->data : Json{{"error", err}});
    } else if (op == "feed") {
      emit("feed", log.log_feed(t).data);
    } else if (op == "hint") {
      emit("hint", log.hint(t));
    } else if (op == "tick") {
      Json evs = Json::array();
      for (const auto& ev : log.tick(t, c.value("score", 0.0), none, none)) evs.push_back(ev.data);
      emit("tick", Json{{"t", t}, {"events", evs}});
    } else if (op == "snapshot") {
      emit("snapshot", log.snapshot(t));
    }
  }
  return 0;
}
