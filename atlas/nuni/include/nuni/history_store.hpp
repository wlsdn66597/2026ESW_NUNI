#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "nuni/json_util.hpp"
namespace nuni {
// All public times are Unix seconds, intervals are [from, to).
// Disk I/O runs on a single worker; observe() never waits for SQLite.
//
// What is kept (2026-11-05 decision):
//  - room state: one temperature/humidity/CO2 summary per `bucket_s` (1 h by default, 2 h allowed),
//    plus how much of that time the radar saw the baby (for the "calm room" baseline)
//  - cry episodes with how they ended, and the room state when each cry started
//  - events only when something changes (alerts, ToF, soothe, vibration, appliance on/off)
// Radar/microphone per-minute statistics and 1-2 s periodic control messages are not stored.
class HistoryStore {
 public:
  struct Config {
    std::string path = "/data/nuni/nuni.db";
    int retention_days = 30;
    int max_mib = 64;
    int bucket_s = 3600;  // room-state summary length: 3600 or 7200
    int flush_s = 30;     // commit at most this often (SD card wear); queries flush first
    std::function<double()> now;
  };
  explicit HistoryStore(Config config);
  ~HistoryStore();
  HistoryStore(const HistoryStore&) = delete;
  HistoryStore& operator=(const HistoryStore&) = delete;
  bool observe(const Json& sample, double received_at, bool retained = false);
  // Room-state summaries. Flushes observations queued before this call, including the current partial bucket.
  Json query(int64_t from, int64_t to, uint32_t limit = 1000);
  bool record(const std::string& topic,const Json& sample,double received_at,bool retained=false);
  Json summary(int64_t from,int64_t to,uint32_t limit=1000);
  Json events(int64_t from,int64_t to,uint32_t limit=1000,int64_t after_id=0);
  Json import_cry_log(const std::string& path);
 private:
  Json request(const std::string& kind,int64_t from,int64_t to,uint32_t limit,int64_t after=0);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
