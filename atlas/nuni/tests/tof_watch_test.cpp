#include "nuni/tof_watch.hpp"
#include <cstdlib>
#include <iostream>

using namespace nuni;

void require(bool ok, const char* message) {
  if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

Json sample(int mm, bool valid = true) {
  return Json{{"distance_mm", mm}, {"valid", valid}};
}

int main() {
  TofWatch w;
  double now = 1000;
  auto feed = [&](int mm, int count, bool valid = true) {
    int events = 0;
    for (int i = 0; i < count; ++i)
      if (w.on_tof(sample(mm, valid), ++now)) ++events;
    return events;
  };
  require(feed(700, 10) == 0, "empty rail must not trigger");
  require(feed(400, 2) == 0, "brief crossing must not trigger");
  require(feed(700, 6) == 0, "clear the beam");
  require(feed(420, 10) == 1, "sustained crossing triggers once");
  require(feed(700, 6) == 0, "rearm requires clear readings");
  require(feed(420, 10) == 0, "cooldown suppresses another crossing");
  require(feed(700, 200) == 0, "empty during cooldown");
  require(feed(420, 5) == 1, "new crossing after cooldown triggers");

  TofWatch stale;
  for (int i = 0; i < 6; ++i) stale.on_tof(sample(700), 2000 + i);
  require(!stale.on_tof(sample(400), 2006), "first occupied frame");
  require(!stale.on_tof(sample(400), 2007), "second occupied frame");
  require(!stale.on_tof(sample(400), 2020), "gap resets consecutive frames");
  require(!stale.on_tof(sample(-1, false), 2021), "invalid range must not trigger");

  TofWatch open;
  for (int i = 0; i < 6; ++i) require(!open.on_tof(sample(-1, false), 3000+i), "invalid background must not trigger");
  int events = 0;
  for (int i = 0; i < 5; ++i) if (open.on_tof(sample(500), 3010+i)) ++events;
  require(events == 1, "valid crossing with out-of-range background");
  std::cout << "PASS: ToF baseline, brief crossing, cooldown, rearm, stale gap, invalid and open background\n";
}
