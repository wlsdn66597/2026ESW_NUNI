#pragma once
#include "nuni/json_util.hpp"
#include <sqlite3.h>
#include <map>
#include <string>
#include <vector>
namespace nuni {
// Private, worker-thread-only extension of HistoryStore. Shares its transaction.
class HistoryRecords {
 public:
  explicit HistoryRecords(sqlite3* db);
  void ingest(const std::string& topic,const Json& sample,double at);
  void environment(const Json& sample,double at);
  void write(double cutoff);
  void committed();
  bool dirty() const;
  bool has_events() const;
  Json summary(int64_t from,int64_t to,uint32_t limit);
  Json sensors(int64_t from,int64_t to,uint32_t limit);
  Json events(int64_t from,int64_t to,uint32_t limit,int64_t after);
  size_t import_file(const std::string& path,double now,int retention_days);
  void make_room(bool force=false);
  bool limited(const std::string& table,int64_t from);
 private:
  sqlite3* db_;
  std::map<std::string,std::pair<int64_t,double>> sources_;
  std::map<std::pair<std::string,int64_t>,Json> minutes_;
  std::map<int64_t,Json> quiet_;
  std::map<std::string,Json> episodes_;
  struct Event {std::string topic,payload;double at;};
  std::map<std::string,Event> events_;
  std::string receipt_nonce_;
  uint64_t receipt_seq_=0;
  double quiet_start_=-1,last_audio_=-1;
  bool audio_quiet_=false;
  std::vector<std::string> written_events_;
  bool fresh(const std::string& topic,const Json& sample,double at);
  void episode(const Json& event,double at);
};
}
