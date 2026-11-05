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
  HistoryRecords(sqlite3* db,int bucket_s);
  void ingest(const std::string& topic,const Json& sample,double at);
  // Latest valid room reading, attached to a cry episode when it starts.
  void environment(const Json& sample,double at);
  void write(double cutoff);
  void committed();
  bool dirty() const;
  bool has_events() const;
  Json summary(int64_t from,int64_t to,uint32_t limit);
  Json events(int64_t from,int64_t to,uint32_t limit,int64_t after);
  size_t import_file(const std::string& path,double now,int retention_days);
  void make_room(bool force=false);
  bool limited(const std::string& table,int64_t from);
 private:
  sqlite3* db_;
  int bucket_s_;
  std::map<std::string,Json> episodes_;
  struct Event {std::string topic,payload;double at;};
  std::map<std::string,Event> events_;
  // Periodic control topics: last stored state (volatile fields removed); unchanged repeats are skipped.
  std::map<std::string,std::string> last_state_;
  std::string receipt_nonce_;
  uint64_t receipt_seq_=0;
  struct Room {double temp=0,humidity=0,co2=0,at=-1;};
  Room room_;
  std::vector<std::string> written_events_;
  void episode(const Json& event,double at);
};
}
