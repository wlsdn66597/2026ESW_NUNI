#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "nuni/json_util.hpp"
namespace nuni {
// All public times are Unix seconds, intervals are [from, to).
// Disk I/O runs on a single worker; observe() never waits for SQLite.
class HistoryStore {
 public:
  struct Config {
    std::string path = "/data/nuni/nuni.db";
    int retention_days = 30;
    int max_mib = 64;
    std::function<double()> now;
  };
  explicit HistoryStore(Config config);
  ~HistoryStore();
  HistoryStore(const HistoryStore&) = delete;
  HistoryStore& operator=(const HistoryStore&) = delete;
  bool observe(const Json& sample, double received_at, bool retained = false);
  // Flushes observations queued before this call, including the current partial minute.
  Json query(int64_t from, int64_t to, uint32_t limit = 1000);
  bool record(const std::string& topic,const Json& sample,double received_at,bool retained=false);
  Json summary(int64_t from,int64_t to,uint32_t limit=1000);
  Json sensor_history(int64_t from,int64_t to,uint32_t limit=1000);
  Json events(int64_t from,int64_t to,uint32_t limit=1000,int64_t after_id=0);
  Json import_cry_log(const std::string& path);
 private:
  Json request(const std::string& kind,int64_t from,int64_t to,uint32_t limit,int64_t after=0);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
