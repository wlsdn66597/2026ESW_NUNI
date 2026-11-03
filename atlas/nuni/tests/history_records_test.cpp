#include "nuni/history_store.hpp"
#include <sqlite3.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using nuni::HistoryStore;using nuni::Json;
static Json env(int seq,double t=23,double h=45){return Json{{"sample_id","env:"+std::to_string(seq)},{"source","pi4-usb"},{"age_s",0},{"valid",true},{"temp",t},{"humidity",h},{"co2",650}};}
int main(){
  const auto dir=std::filesystem::temp_directory_path()/("nuni-records-"+std::to_string(getpid()));std::filesystem::create_directories(dir);
  const int64_t base=1800000000;
  HistoryStore::Config cfg;cfg.path=(dir/"records.db").string();cfg.now=[=]{return double(base+2000);};
  {
    HistoryStore h(cfg);
    auto event=[&](const char* topic,Json j,double at){assert(h.record(topic,j,at));};
    event("cry/episode",Json{{"event","start"},{"id","one"},{"t0",base+2}},base+2);
    event("cry/episode",Json{{"event","end"},{"id","one"},{"t0",base+2},{"t1",base+5}},base+5);
    Json answer{{"event","answer"},{"id","one"},{"action","feed"},{"ts",base+6}};
    event("cry/feedback",answer,base+6);
    event("cry/feedback",Json{{"ts",base+6},{"action","feed"},{"id","one"},{"event","answer"}},base+6.1); // echo, reordered keys
    event("cry/feedback",Json{{"event","not"},{"id","one"},{"action","diaper"},{"ts",base+7}},base+7);
    event("cry/episode",Json{{"event","end"},{"id","two"},{"t0",base+10},{"t1",base+20}},base+20);
    event("cry/episode",Json{{"event","start"},{"id","noise"},{"t0",base+30}},base+30);
    event("cry/episode",Json{{"event","drop"},{"id","noise"},{"t0",base+30}},base+31);
    event("cry/hint",Json{{"id","one"},{"probs",Json{{"feed",0.8}}}},base+3);
    event("fusion/alert",Json{{"ts",base+8},{"level","attention"}},base+8);
    event("event/tof",Json{{"id","rail-1"},{"event","rail_intrusion"}},base+9);
    event("control/soothe",Json{{"id","rail-1"},{"status","completed"}},base+11);
    // Unidentified, identical events are independent receipts, not discarded as echoes.
    event("control/custom",Json{{"on",true}},base+12);
    event("control/custom",Json{{"on",true}},base+13);
    assert(!h.record("control/action",Json{{"ts",base+1}},base+1,true));
    assert(!h.record("unrelated/topic",Json::object(),base));
    Json radar{{"boot_id","radar-boot"},{"seq",1},{"source","pi4-usb"},{"valid",true},{"presence",true},{"breath_valid",true},{"breathing_rate",30},{"movement",0.2}};
    event("sensor/radar",radar,base+1);event("sensor/radar",radar,base+2);
    radar["seq"]=2;radar["breath_valid"]=false;radar["breathing_rate"]=99;radar["apnea"]=true;event("sensor/radar",radar,base+3);
    // At least ten continuous minutes of positively observed, non-crying audio.
    int seq=0,envseq=0;
    for(int sec=60;sec<=840;++sec){
      event("audio/cry",Json{{"boot_id","mic-1"},{"seq",++seq},{"ts",base+sec},{"is_crying",false},{"confidence",0.1}},base+sec);
      if(sec==61){radar["seq"]=3;event("sensor/radar",radar,base+sec);}
      if(sec==660)h.observe(env(++envseq,22.2,46),base+sec);
      if(sec==720)h.observe(env(++envseq,23.8,51),base+sec);
      if(sec==780)h.observe(env(++envseq,23,48),base+sec);
    }
    event("audio/cry",Json{{"boot_id","mic-1"},{"seq",++seq},{"ts",base+900},{"is_crying",false},{"confidence",0.1}},base+900);
    h.observe(env(++envseq,99,99),base+901); // 60-second audio gap must reset quiet time
    auto summary=h.summary(base,base+2001,1);
    assert(summary["ok"]==true);assert(summary["total_count"]==2);assert(summary["truncated"]==true);
    assert(summary["hourly"][(base/3600+9)%24]==2);
    assert(summary["episodes"][0]["id"]=="two");
    assert(summary["quiet"]["count"]==3);assert(summary["quiet"]["temperature_c"]["min"]==22.2);
    assert(summary["quiet"]["temperature_c"]["max"]==23.8);
    assert(h.summary(base,base+2001)["episodes"][1]["action"]=="feed");
    auto sensors=h.sensor_history(base,base+60);
    assert(sensors["records"].size()==1);
    const auto stats=sensors["records"][0]["stats"];
    assert(stats["sample_count"]==2);assert(stats["breathing_rate"]["count"]==1);assert(stats["apnea_count"]==1);
    assert(h.sensor_history(base+60,base+120)["records"][0]["stats"]["sample_count"]==60);
    const auto sensor_page=h.sensor_history(base+60,base+180,1);
    assert(sensor_page["records"].size()==2);assert(sensor_page["truncated"]==true);
    assert(sensor_page["next_from_s"]==base+120);
    const auto events=h.events(base,base+2001);assert(events["records"].size()==13);
    auto page=h.events(base,base+2001,2);assert(page["truncated"]==true);
    auto next=h.events(base,base+2001,1000,page["after_id"]);assert(next["records"].size()==11);
    assert(h.events(base,base+2001,1001)["ok"]==false);
    event("cry/episode",Json{{"event","end"},{"id","future"},{"t0",base+10},{"t1",base+3000}},base+100);
    assert(h.summary(base,base+2001)["total_count"]==2);
    {std::ofstream malformed(dir/"broken.json");malformed<<"not JSON";}
    assert(h.import_cry_log((dir/"broken.json").string())["ok"]==false);
    assert(h.summary(base,base+2001)["total_count"]==2);
    std::ofstream file(dir/"cry_log.json");
    file<<Json{{"episodes",Json::array({Json{{"id","three"},{"t0",base+50},{"t1",base+80},{"action","hold"},{"answered_at",base+90}},
      Json{{"id","one"},{"t0",base+2},{"t1",base+4},{"action","diaper"},{"answered_at",0}}})}}.dump();file.close();
    assert(h.import_cry_log((dir/"cry_log.json").string())["ok"]==true);
    assert(h.import_cry_log((dir/"cry_log.json").string())["ok"]==true);
    summary=h.summary(base,base+2001);assert(summary["total_count"]==3);assert(summary["episodes"][2]["action"]=="feed");assert(summary["episodes"][2]["t1"]==base+5);
    event("cry/feedback",Json{{"event","answer"},{"id","three"},{"ts",base+100},{"action","diaper"}},base+100);
    event("cry/feedback",Json{{"event","answer"},{"id","three"},{"ts",base+95},{"action","feed"}},base+101);
    assert(h.summary(base,base+2001)["episodes"][0]["action"]=="diaper");
  }
  {
    HistoryStore h(cfg);assert(h.summary(base,base+2001)["total_count"]==3);
    // Restart conservatively requires a new continuous audio interval.
    h.observe(env(5,100,100),base+902);
    assert(h.summary(base,base+2001)["quiet"]["count"]==3);
  }
  cfg.now=[=]{return double(base+40*86400);};
  {
    HistoryStore h(cfg);assert(h.summary(base,base+2001)["total_count"]==0);
    assert(h.events(base,base+2001)["records"].empty());assert(h.sensor_history(base,base+2001)["records"].empty());
  }
  // New schema preserves pre-existing environment data.
  cfg.path=(dir/"old.db").string();cfg.now=[=]{return double(base+2000);};sqlite3* db=nullptr;
  assert(sqlite3_open(cfg.path.c_str(),&db)==SQLITE_OK);
  const std::string old="CREATE TABLE environment_minutes(minute_s INTEGER PRIMARY KEY,total_count INTEGER NOT NULL,valid_count INTEGER NOT NULL,temp_sum REAL,temp_min REAL,temp_max REAL,humidity_sum REAL,humidity_min REAL,humidity_max REAL,co2_sum REAL,co2_min REAL,co2_max REAL);"
    "CREATE TABLE environment_sources(source TEXT PRIMARY KEY,sequence INTEGER NOT NULL);PRAGMA user_version=1;INSERT INTO environment_minutes VALUES("+std::to_string(base)+",1,1,22,22,22,45,45,45,650,650,650);";
  assert(sqlite3_exec(db,old.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(db);
  {HistoryStore h(cfg);assert(h.query(base,base+60)["records"][0]["temperature_c"]["mean"]==22);}
  // Event bursts exceeding a small DB cap are chunked; the newest data remains queryable.
  cfg.path=(dir/"capped.db").string();cfg.max_mib=4;
  {HistoryStore h(cfg);
   h.observe(env(1),base+1);
   h.record("cry/episode",Json{{"event","end"},{"id","protected"},{"t0",base+1},{"t1",base+2}},base+2);
   for(int i=0;i<800;++i)h.record("control/test",Json{{"id",i},{"ts",base+i},{"blob",std::string(8192,'x')}},base+i);
   auto result=h.events(base,base+2001);assert(result["ok"]==true);assert(!result["records"].empty());assert(result["limited_by_size"]==true);
   bool newest=false;for(const auto& row:result["records"])if(row["payload"]["id"]==799)newest=true;assert(newest);
   const auto protected_summary=h.summary(base,base+2001);
   assert(protected_summary["total_count"]==1);assert(protected_summary["limited_by_size"]==false);
   assert(h.query(base,base+60)["records"].size()==1);
  }
  assert(std::filesystem::file_size(cfg.path)<=4*1024*1024);
  std::filesystem::remove_all(dir);std::cout<<"history records: summaries, quiet coverage, event dedup, JSON import, migration, restart and size cap OK\n";
}
