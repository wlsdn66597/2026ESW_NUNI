#include "nuni/history_store.hpp"
#include <cassert>
#include <sqlite3.h>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <thread>
#include <unistd.h>
using nuni::HistoryStore;
using nuni::Json;
static Json sample(int seq,double t=24,bool valid=true) {
  return Json{{"sample_id","esp:"+std::to_string(seq)},{"source","pi4-usb"},
    {"seq",seq},{"boot_id","hub"},{"valid",valid},{"age_s",0},
    {"temp",t},{"humidity",45},{"co2",650}};
}
int main() {
  const auto dir=std::filesystem::temp_directory_path()/("nuni-history-"+std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  double now=1800000000; // minute aligned
  HistoryStore::Config cfg;
  cfg.path=(dir/"history.db").string();cfg.now=[&]{return now;};
  {
    HistoryStore h(cfg);
    assert(h.query(0,0)["ok"]==false);
    assert(h.query(-1,now)["ok"]==false);
    assert(h.query(now,now+60,0)["ok"]==false);
    assert(h.query(now,now+32*86400)["ok"]==false);
    assert(h.query(now,now+60)["records"].empty());
    h.observe(sample(1,23),now+1);h.observe(sample(2,24),now+2);h.observe(sample(3,25),now+3);
    // Repeated hub publications do not constitute new ESP measurements.
    auto repeated=sample(3,99);repeated["seq"]=200;h.observe(repeated,now+4);
    h.observe(sample(2,99),now+5); // out-of-order source
    assert(!h.observe(sample(4,99),now+6,true)); // retained
    h.observe(sample(4,99,false),now+7);
    auto stale=sample(5);stale["age_s"]=21;h.observe(stale,now+8);
    auto invalid=sample(6);invalid["humidity"]="bad";h.observe(invalid,now+9);
    auto result=h.query(now,now+60);
    assert(result["ok"]==true);assert(result["records"].size()==1);
    auto row=result["records"][0];
    assert(row["total_count"]==6);assert(row["valid_count"]==3);
    assert(row["temperature_c"]["mean"]==24);assert(row["temperature_c"]["min"]==23);
    assert(row["temperature_c"]["max"]==25);
    h.observe(sample(7,26),now+59.99); // same partial minute checkpoint
    h.observe(sample(8,27),now+60); // next minute boundary
    assert(h.query(now,now+60)["records"][0]["valid_count"]==4);
    auto limited=h.query(now,now+120,1);
    assert(limited["truncated"]==true);assert(limited["records"].size()==1);
    assert(h.query(now+60,now+120)["records"][0]["temperature_c"]["mean"]==27);
    // A busy database reports an error and retains the unsaved aggregate for retry.
    sqlite3* locked=nullptr;
    assert(sqlite3_open(cfg.path.c_str(),&locked)==SQLITE_OK);
    assert(sqlite3_exec(locked,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);
    h.observe(sample(8,99),now+60); // duplicate must not alter the result
    assert(h.query(now,now+120)["ok"]==false);
    assert(sqlite3_exec(locked,"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK);
    sqlite3_close(locked);
    assert(h.query(now,now+120)["ok"]==true);
    auto invalid2=sample(9);invalid2["temp"]=nullptr;h.observe(invalid2,now+120);
    auto empty=h.query(now+120,now+180)["records"][0];
    assert(empty["total_count"]==1 && empty["valid_count"]==0);
    assert(empty["temperature_c"].is_null());
  }
  {
    HistoryStore h(cfg);
    // Closing and reopening restores the data and source checkpoints.
    h.observe(sample(9,99),now+120);
    assert(h.query(now+120,now+180)["records"][0]["total_count"]==1);
    h.observe(sample(10,28),now+60);
    assert(h.query(now+60,now+120)["records"][0]["temperature_c"]["mean"]==27.5);
    // Independent writers and queries share one SQLite worker.
    std::thread writer([&]{for(int i=11;i<=100;++i) h.observe(sample(i,20),now+180);});
    for(int i=0;i<10;++i) assert(h.query(now,now+240)["ok"]==true);
    writer.join();
    assert(h.query(now+180,now+240)["records"][0]["valid_count"]==90);
  }
  now+=31*86400;
  {
    HistoryStore h(cfg);
    assert(h.query(now-31*86400,now)["records"].empty());
    h.observe(sample(101,22),now+1); // destructor flush without explicit query
  }
  {
    HistoryStore h(cfg);
    assert(h.query(now,now+60)["records"][0]["temperature_c"]["mean"]==22);
  }
  std::filesystem::remove_all(dir);
  std::cout<<"environment history: aggregation, invalid/stale/duplicate data, retention, restart, concurrency OK\n";
}
