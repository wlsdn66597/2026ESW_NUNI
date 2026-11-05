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
  double now=1800000000; // hour aligned
  const double H=3600;
  HistoryStore::Config cfg;
  cfg.path=(dir/"history.db").string();cfg.now=[&]{return now;};
  {
    HistoryStore h(cfg);
    assert(h.query(0,0)["ok"]==false);
    assert(h.query(-1,now)["ok"]==false);
    assert(h.query(now,now+H,0)["ok"]==false);
    assert(h.query(now,now+32*86400)["ok"]==false);
    assert(h.query(now,now+H)["records"].empty());
    h.observe(sample(1,23),now+1);h.observe(sample(2,24),now+2);h.observe(sample(3,25),now+3);
    // Repeated hub publications do not constitute new ESP measurements.
    auto repeated=sample(3,99);repeated["seq"]=200;h.observe(repeated,now+4);
    h.observe(sample(2,99),now+5); // out-of-order source
    assert(!h.observe(sample(4,99),now+6,true)); // retained
    h.observe(sample(4,99,false),now+7);
    auto stale=sample(5);stale["age_s"]=21;h.observe(stale,now+8);
    auto invalid=sample(6);invalid["humidity"]="bad";h.observe(invalid,now+9);
    auto result=h.query(now,now+H);
    assert(result["ok"]==true);assert(result["bucket_seconds"]==3600);assert(result["records"].size()==1);
    auto row=result["records"][0];
    assert(row["bucket_s"]==now);
    assert(row["total_count"]==6);assert(row["valid_count"]==3);
    assert(row["temperature_c"]["mean"]==24);assert(row["temperature_c"]["min"]==23);
    assert(row["temperature_c"]["max"]==25);
    assert(row["presence_ratio"].is_null()); // no radar publications yet
    h.observe(sample(7,26),now+H-0.01); // same hour, later minute: one row, not one per minute
    h.observe(sample(8,27),now+H);      // next hour boundary
    assert(h.query(now,now+H)["records"][0]["valid_count"]==4);
    auto limited=h.query(now,now+2*H,1);
    assert(limited["truncated"]==true);assert(limited["records"].size()==1);
    assert(h.query(now+H,now+2*H)["records"][0]["temperature_c"]["mean"]==27);
    // A busy database reports an error and retains the unsaved aggregate for retry.
    sqlite3* locked=nullptr;
    assert(sqlite3_open(cfg.path.c_str(),&locked)==SQLITE_OK);
    assert(sqlite3_exec(locked,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);
    h.observe(sample(8,99),now+H); // duplicate must not alter the result
    assert(h.query(now,now+2*H)["ok"]==false);
    assert(sqlite3_exec(locked,"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK);
    sqlite3_close(locked);
    assert(h.query(now,now+2*H)["ok"]==true);
    auto invalid2=sample(9);invalid2["temp"]=nullptr;h.observe(invalid2,now+2*H);
    auto empty=h.query(now+2*H,now+3*H)["records"][0];
    assert(empty["total_count"]==1 && empty["valid_count"]==0);
    assert(empty["temperature_c"].is_null());
    // Radar only counts how much of the hour the baby was in view.
    Json radar{{"boot_id","pi4"},{"seq",1},{"valid",true},{"presence_valid",true},{"presence",true}};
    for(int i=1;i<=4;++i){radar["seq"]=i;radar["presence"]=i!=4;h.record("sensor/radar",radar,now+2*H+i);}
    radar["seq"]=4;h.record("sensor/radar",radar,now+2*H+5); // repeated publication
    auto presence=h.query(now+2*H,now+3*H)["records"][0];
    assert(presence["radar_count"]==4);assert(presence["presence_ratio"]==0.75);
  }
  {
    HistoryStore h(cfg);
    // Closing and reopening restores the data and source checkpoints.
    h.observe(sample(9,99),now+2*H);
    assert(h.query(now+2*H,now+3*H)["records"][0]["total_count"]==1);
    h.observe(sample(10,28),now+H);
    assert(h.query(now+H,now+2*H)["records"][0]["temperature_c"]["mean"]==27.5);
    // Independent writers and queries share one SQLite worker.
    std::thread writer([&]{for(int i=11;i<=100;++i) h.observe(sample(i,20),now+3*H);});
    for(int i=0;i<10;++i) assert(h.query(now,now+4*H)["ok"]==true);
    writer.join();
    assert(h.query(now+3*H,now+4*H)["records"][0]["valid_count"]==90);
  }
  // Switching to 2-hour summaries folds the existing hourly rows.
  cfg.bucket_s=7200;
  {
    HistoryStore h(cfg);
    const auto rows=h.query(now,now+4*H)["records"];
    assert(rows.size()==2);assert(rows[0]["bucket_s"]==now);assert(rows[0]["valid_count"]==6);
    assert(rows[1]["bucket_s"]==now+2*H);assert(rows[1]["valid_count"]==90);assert(rows[1]["radar_count"]==4);
  }
  cfg.bucket_s=3600;
  now+=31*86400;
  {
    HistoryStore h(cfg);
    assert(h.query(now-31*86400,now)["records"].empty());
    h.observe(sample(101,22),now+1); // destructor flush without explicit query
  }
  {
    HistoryStore h(cfg);
    assert(h.query(now,now+H)["records"][0]["temperature_c"]["mean"]==22);
  }
  std::filesystem::remove_all(dir);
  std::cout<<"environment history: hourly aggregation, presence ratio, invalid/stale/duplicate data, re-bucketing, retention, restart, concurrency OK\n";
}
