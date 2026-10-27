// JSON 헬퍼 — Python dict 동작(dict.get, 참/거짓 판정, round)을 C++에서 똑같이 흉내 낸다.
//
// 원본 Python 코드는 `radar.get("movement", 0.0)`, `if radar:`, `round(x, 2)` 같은 표현을 쓴다.
// 이식 결과가 Python과 같은 판단을 내리도록 그 의미를 그대로 옮긴 함수들이다.
#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace nuni {

// 키 순서를 유지하는 JSON(Python dict의 삽입 순서와 같게 발행하려고).
using Json = nlohmann::ordered_json;

// Python의 `bool(x)`: None·False·0·빈 문자열·빈 배열·빈 객체는 거짓.
bool truthy(const Json& v);

// Python의 `obj.get(key, default)`: 키가 있으면 값이 null이어도 그 값을, 없으면 default.
// obj가 객체가 아니면 default.
Json pyget(const Json& obj, const char* key, const Json& def = nullptr);

// 숫자 값. 숫자·불(True=1)만 숫자로 보고, 그 밖(null·문자열 등)은 nullopt.
std::optional<double> as_number(const Json& v);

// 숫자 값, 숫자가 아니면 fallback.
// (Python 원본은 None과 크기 비교하면 TypeError로 멈춘다. C++은 멈추지 않고 fallback을 쓴다.)
double num_or(const Json& v, double fallback);

// Python의 round(x, ndigits): 정확한 2진 값을 기준으로 반올림하고, 딱 절반이면 짝수 쪽으로.
// glibc printf("%.nf")가 같은 규칙이라 그것으로 계산한다.
double py_round(double x, int ndigits);

// Python의 f-string `{x}` 표기(문자열은 그대로, None/True/False, 정수, 실수).
std::string py_str(const Json& v);

// Python json.dumps가 내보내는 NaN·Infinity·-Infinity(표준 JSON 아님)를 null로 바꾼다.
// Python 쪽 발행자(예: 기존 radar_bridge.py)가 섞여 있어도 C++에서 읽을 수 있게 한다.
std::string sanitize_nonfinite(const std::string& raw);

// MQTT 페이로드를 JSON으로 읽는다. 실패하면 nullopt(비JSON 문자열 등).
std::optional<Json> parse_payload(const std::string& raw);

// 발행용 직렬화. Python json.dumps(ensure_ascii=True)처럼 비ASCII는 \uXXXX로 내보낸다.
// NaN·무한대는 null로 나간다(표준 JSON. Flutter jsonDecode도 읽을 수 있음).
std::string dump(const Json& v);

// 현재 시각(초, 소수점 포함) — Python time.time().
double wall_now();

}  // namespace nuni
