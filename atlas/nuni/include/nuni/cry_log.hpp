// 울음 기록과 울음 도우미 힌트 — cry_personal.py(CryPersonal)의 흐름을 atlas 서비스 안으로 옮긴 것.
//
// 흐름(CryPersonal과 같은 이름)
//   start / add_window / end  울음 구간을 열고, 울음 1초마다 그 창의 YAMNet 임베딩을 모으고, 닫는다.
//                             tick()이 1초마다 울음 점수를 받아 이 셋을 알아서 부른다(울음 구간 나누기).
//   hint                      울음이 시작되면 먼저 해 볼 1·2순위를 낸다.
//   feedback                  부모의 "무엇으로 그쳤나요?" 답. 수유면 마지막 수유 시각으로도 쓴다.
//   feedback_not              "해 봤는데 안 그쳤어요". log_feed는 대시보드에서 따로 기록한 수유.
// 힌트 = 맥락(시간대·직전 울음 후 시간·마지막 수유 후 시간, 공통 로지스틱 회귀) × 이 아기의 이전 답 빈도
//   (최근 답일수록 크게). cry_personal.py에서 자가 검증이 소리를 끈 상태(sound_on=False)와 같은 계산이다
//   (tests/cry_log_parity.py로 Python과 비교). 실제 아기 기록(Enes) 재생에서도 자가 검증은 소리를 대부분 꺼 두었다
//   (cry_model/README.md). 소리 비교(블록 특징 + 아기별 판별 분석 + 자가 검증)는 아직 옮기지 않았다. 대신 울음마다
//   소리(16kHz WAV)와 1초 창 임베딩을 남겨, 소리 모델을 옮긴 뒤 지금까지 쌓은 답으로 바로 다시 배울 수 있게 한다.
//
// 저장(dir, 기본 /data/nuni/cry — 패키지를 다시 깔아도 지워지지 않는 곳)
//   cry_log.json      울음 구간·답·미리 낸 예측·마지막 수유(답할 때마다 통째로 다시 쓴다)
//   audio/<id>.wav    울음 구간 소리(16kHz 16비트 모노, 시작 전 preroll_s초 포함). max_audio_mb를 넘으면 오래된 것부터 지운다
//   emb/<id>.f32      울음으로 판정된 1초마다 YAMNet 임베딩 1024개(float32, 이어 붙임)
// 소리는 이 기기 안에만 남는다(밖으로 보내지 않는다). record=false면 소리·임베딩 없이 답만 쌓는다.
#pragma once

#include <array>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "nuni/json_util.hpp"

namespace nuni {

struct CryLogConfig {
  std::string dir = "/data/nuni/cry";
  std::string ctx_path;                                        // cry_personal_ctx.json. 없으면 맥락은 균등
  std::vector<std::string> actions{"feed", "diaper", "hold"};  // cry_personal.ACTIONS3 (1·2순위로 보여 준다)
  double min_score = 0.3;   // 이 점수 이상인 1초를 울음으로 본다(audio/cry의 is_crying과 같은 기준)
  double end_gap_s = 8.0;   // 울음 판정이 이만큼 끊기면 울음이 끝난 것으로 본다
  int min_windows = 3;      // 울음으로 판정된 1초가 이보다 적은 구간은 잡음으로 버린다
  int hint_seconds = 10;    // CryPersonal.hint_seconds
  int k_min = 10;           // 답이 이만큼 쌓여야 힌트를 보여 준다(CryPersonal.k_min)
  double show_min = 0.5;    // CryPersonal.show_min
  double show2_min = 0.0;   // CryPersonal.show2_min
  double half_life = 8.0;   // CryPersonal.half_life(이력: 답 8개마다 무게 절반)
  int check_window = 20;    // 대시보드에 보일 '최근 답에서 미리 낸 예측이 맞은 수'를 셀 답 개수
  bool record = true;       // 소리·임베딩 저장
  double max_audio_mb = 2000;
  double max_audio_s = 600;  // 울음 하나에 저장하는 소리의 최대 길이
  double preroll_s = 2.0;    // 울음 시작 전 소리도 이만큼 붙인다
  double pending_hours = 12; // 이보다 오래된 답 없는 울음은 다시 묻지 않는다
};

// 맥락 모델(cry_personal_ctx.json, sim_personal.py ctx): 다항 로지스틱 회귀
struct CryContextModel {
  std::vector<std::vector<double>> coef;  // [클래스][특징 4]
  std::vector<double> intercept, mean, std;
  std::vector<std::string> classes;
};
std::optional<CryContextModel> load_cry_context(const std::string& path);

// cry_personal.ctx_features: 하루 중 시각(sin·cos, 기기 현지 시각), 직전 울음 후 시간, 마지막 수유 후 시간
// (시간 단위, 0~12. 모르면 12)
std::array<double, 4> cry_ctx_features(double t, std::optional<double> last_cry, std::optional<double> last_feed);

// cry_personal.LABEL_KO
std::string cry_label_ko(const std::string& action);

// 기록만 하는 답(cry_personal.EXTRA, 대시보드 버튼 순서)
inline const std::vector<std::string>& cry_extra_answers() {
  static const std::vector<std::string> v{"self", "sick", "other", "unknown"};
  return v;
}

class CryLog {
 public:
  struct Event {
    std::string kind;  // start · hint · end · drop · answer · not · feed
    Json data;         // {"event": kind, "id": ..., ...}
  };

  explicit CryLog(CryLogConfig cfg);
  ~CryLog();
  CryLog(const CryLog&) = delete;
  CryLog& operator=(const CryLog&) = delete;

  // 1초마다: 그 1초의 울음 점수, YAMNet 임베딩(1024, 없으면 빈 벡터), 지난 1초 소리(16kHz).
  // 울음 구간을 나눠 start / add_window / end를 부르고, 그때 생긴 사건을 돌려준다.
  std::vector<Event> tick(double t, double score, const std::vector<float>& emb, const std::vector<float>& audio16k);

  // CryPersonal과 같은 단계 호출(tick이 쓰고, 시험 재생 도구도 쓴다)
  std::vector<Event> start(double t);
  void add_window(const std::vector<float>& emb, const std::vector<float>& audio16k, double score = 1.0);
  std::optional<Event> end(double t);  // 울음 1초가 min_windows보다 적으면 drop 사건

  // 부모 답. id가 없거나 답 이름이 틀리면 nullopt와 error.
  std::optional<Event> feedback(const std::string& id, const std::string& action, double now, std::string* error = nullptr);
  std::optional<Event> feedback_not(const std::string& id, const std::string& action, double now,
                                    std::string* error = nullptr);
  Event log_feed(double t);

  // 지금 울음이 t에 시작했다면 낼 힌트(CryPersonal.hint(t, embs=[])와 같은 키·같은 값)
  Json hint(double t) const;
  // 대시보드용 전체 상태(최근 recent개 울음 포함)
  Json snapshot(double now, size_t recent = 40) const;

  const CryLogConfig& config() const { return cfg_; }

 private:
  struct Episode {
    std::string id;
    double t0 = 0, t1 = 0;
    int n = 0;
    double peak = 0, mean = 0;
    std::string action;              // 비어 있으면 아직 답 없음
    std::vector<std::string> nots;   // 해 봤는데 안 그친 행동
    std::array<double, 4> c{};       // 울음 시작 때 맥락 특징(아기별 맥락 학습용, CryPersonal "c")
    std::string pred_base;           // 답 받기 전에 미리 낸 1순위(맥락 × 이력)
    Json hint;                       // 울음 시작 때 낸 힌트(top2·probs·show·show2)
    double answered_at = 0;
    std::string audio, emb;          // 저장한 파일(dir 기준 상대 경로, 없으면 빈 문자열)
  };
  struct Current {
    std::string id;
    double t0 = 0, last_cry = 0;
    int n = 0;
    double peak = 0, sum = 0;
    Json hint;
    std::vector<int16_t> audio;
    std::FILE* emb_fp = nullptr;
  };

  // 계산(잠금 안에서 부른다)
  std::vector<double> context_(double t, std::optional<double> last_cry) const;
  std::vector<double> prior_() const;
  std::optional<double> last_cry_() const;
  Json hint_(double t) const;
  Json episode_json_(const Episode& e, bool full) const;
  Episode* find_(const std::string& id);
  bool valid_answer_(const std::string& a) const;
  // 저장
  void load_();
  void save_() const;
  void prune_audio_();
  void append_audio_(const std::vector<float>& audio16k);
  void add_window_(const std::vector<float>& emb, const std::vector<float>& audio16k, double score);
  std::vector<Event> start_(double t);
  std::optional<Event> end_(double t);

  CryLogConfig cfg_;
  std::optional<CryContextModel> ctx_;
  std::vector<Episode> episodes_;
  std::optional<double> last_feed_;
  std::optional<Current> cur_;
  std::deque<int16_t> preroll_;  // 울음 시작 전 소리(16kHz)
  mutable std::mutex mu_;
};

}  // namespace nuni
