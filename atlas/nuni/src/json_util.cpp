#include "nuni/json_util.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace nuni {

bool truthy(const Json& v) {
  switch (v.type()) {
    case Json::value_t::null: return false;
    case Json::value_t::boolean: return v.get<bool>();
    case Json::value_t::number_integer: return v.get<long long>() != 0;
    case Json::value_t::number_unsigned: return v.get<unsigned long long>() != 0;
    case Json::value_t::number_float: return v.get<double>() != 0.0;  // NaN도 참(Python과 같음)
    case Json::value_t::string: return !v.get_ref<const std::string&>().empty();
    case Json::value_t::array:
    case Json::value_t::object: return !v.empty();
    default: return false;
  }
}

Json pyget(const Json& obj, const char* key, const Json& def) {
  if (!obj.is_object()) return def;
  auto it = obj.find(key);
  return it == obj.end() ? def : *it;
}

std::optional<double> as_number(const Json& v) {
  if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
  if (v.is_number()) return v.get<double>();
  return std::nullopt;
}

double num_or(const Json& v, double fallback) {
  auto n = as_number(v);
  return n ? *n : fallback;
}

double py_round(double x, int ndigits) {
  if (!std::isfinite(x)) return x;
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%.*f", ndigits, x);
  return std::strtod(buf, nullptr);
}

std::string py_str(const Json& v) {
  if (v.is_null()) return "None";
  if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
  if (v.is_string()) return v.get<std::string>();
  return v.dump();
}

std::string sanitize_nonfinite(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  bool in_str = false, esc = false;
  for (size_t i = 0; i < raw.size();) {
    char c = raw[i];
    if (in_str) {
      out += c;
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') in_str = false;
      ++i;
      continue;
    }
    if (c == '"') { in_str = true; out += c; ++i; continue; }
    if (raw.compare(i, 3, "NaN") == 0) { out += "null"; i += 3; continue; }
    if (raw.compare(i, 8, "Infinity") == 0) { out += "null"; i += 8; continue; }
    if (raw.compare(i, 9, "-Infinity") == 0) { out += "null"; i += 9; continue; }
    out += c;
    ++i;
  }
  return out;
}

std::optional<Json> parse_payload(const std::string& raw) {
  Json j = Json::parse(sanitize_nonfinite(raw), nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) return std::nullopt;
  return j;
}

std::string dump(const Json& v) {
  return v.dump(-1, ' ', /*ensure_ascii=*/true, Json::error_handler_t::replace);
}

double wall_now() {
  using namespace std::chrono;
  return duration<double>(system_clock::now().time_since_epoch()).count();
}

}  // namespace nuni
