#pragma once
#include <chrono>
namespace nuni {
inline double steady_now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
