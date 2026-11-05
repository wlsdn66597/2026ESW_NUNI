#include "nuni/history_store.hpp"
#include <sqlite3.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using nuni::HistoryStore;using nuni::Json;
static Json env(int seq,double t=23,double h=45,double c=650){return Json{{"sample_id","env:"+std::to_string(seq)},{"source","pi4-usb"},{"age_s",0},{"valid",true},{"temp",t},{"humidity",h},{"co2",c}};}
static int64_t scalar(const std::string& path,const char* sql){
  sqlite3* db=nullptr;assert(sqlite3_open(path.c_str(),&db)==SQLITE_OK);sqlite3_stmt* s=nullptr;
  assert(sqlite3_prepare_v2(db,sql,-1,&s,nullptr)==SQLITE_OK);assert(sqlite3_step(s)==SQLITE_ROW);
  const auto v=sqlite3_column_int64(s,0);sqlite3_finalize(s);sqlite3_close(db);return v;
}
int main(){
  const auto dir=std::filesystem::temp_directory_path()/("nuni-records-"+std::to_string(getpid()));std::filesystem::create_directories(dir);
  const int64_t base=1800000000; // hour aligned
  const int64_t H=3600;
  HistoryStore::Config cfg;cfg.path=(dir/"records.db").string();cfg.now=[=]{return double(base+6*H);};
  {
    HistoryStore h(cfg);
    auto event=[&](const char* topic,Json j,double at){assert(h.record(topic,j,at));};
    // The room reading just before a cry is attached to that cry.
    h.observe(env(1,21.5,31,900),base+1);
    event("cry/episode",Json{{"event","start"},{"id","one"},{"t0",base+2}},base+2);
    event("cry/episode",Json{{"event","end"},{"id","one"},{"t0",base+2},{"t1",base+5}},base+5);
    Json answer{{"event","answer"},{"id","one"},{"action","feed"},{"ts",base+6}};
    event("cry/feedback",answer,base+6);
    event("cry/feedback",Json{{"ts",base+6},{"action","feed"},{"id","one"},{"event","answer"}},base+6.1); // echo, reordered keys
    event("cry/feedback",Json{{"event","not"},{"id","one"},{"action","diaper"},{"ts",base+7}},base+7);
    event("cry/episode",Json{{"event","end"},{"id","two"},{"t0",base+10},{"t1",base+20}},base+20); // start not seen: no room
    event("cry/episode",Json{{"event","start"},{"id","noise"},{"t0",base+30}},base+30);
    event("cry/episode",Json{{"event","drop"},{"id","noise"},{"t0",base+30}},base+31);
    event("cry/hint",Json{{"id","one"},{"probs",Json{{"feed",0.8}}}},base+3);
    event("fusion/alert",Json{{"ts",base+8},{"level","alert"},{"apnea",true},{"apnea_onset",true}},base+8);
    event("event/tof",Json{{"id","rail-1"},{"event","rail_intrusion"}},base+9);
    event("control/soothe",Json{{"id","rail-1"},{"status","completed"}},base+11);
    // Unidentified, identical events are independent receipts, not discarded as echoes.
    event("control/custom",Json{{"on",true}},base+12);
    event("control/custom",Json{{"on",true}},base+13);
    // Periodic control messages: kept only when their state changes.
    Json desired{{"cool",false},{"humidify",true}};
    for(int i=0;i<120;++i)
      event("control/action",Json{{"schema",2},{"desired",desired},{"actuation_enabled",false},{"ts",base+40+i*0.25}},base+40+i*0.25);
    desired["humidify"]=false;
    event("control/action",Json{{"schema",2},{"desired",desired},{"actuation_enabled",false},{"ts",base+80}},base+80);
    for(int i=0;i<30;++i)
      event("control/humidifier/state",Json{{"v",1},{"usb_connected",true},{"actual_on",i>=20},{"command_id",std::to_string(i)},{"ts",base+40+i}},base+40+i);
    // The microphone's 1 Hz results are not stored; radar only counts presence.
    assert(!h.record("audio/cry",Json{{"boot_id","mic"},{"seq",1},{"is_crying",false},{"confidence",0.1}},base+1));
    assert(!h.record("control/action",Json{{"ts",base+1}},base+1,true));
    assert(!h.record("unrelated/topic",Json::object(),base));

    auto summary=h.summary(base,base+6*H,1);
    assert(summary["ok"]==true);assert(summary["total_count"]==2);assert(summary["truncated"]==true);
    assert(summary["hourly"][(base/3600+9)%24]==2);
    assert(summary["episodes"][0]["id"]=="two");assert(summary["episodes"][0]["room"].is_null());
    const auto both=h.summary(base,base+6*H)["episodes"];
    assert(both[1]["action"]=="feed");
    assert(both[1]["room"]["temp"]==21.5);assert(both[1]["room"]["humidity"]==31);assert(both[1]["room"]["co2"]==900);
    // 13 one-off events + 2 control/action states + 2 humidifier states.
    const auto events=h.events(base,base+6*H);assert(events["records"].size()==17);
    auto page=h.events(base,base+6*H,2);assert(page["truncated"]==true);
    auto next=h.events(base,base+6*H,1000,page["after_id"]);assert(next["records"].size()==15);
    assert(h.events(base,base+6*H,1001)["ok"]==false);
    event("cry/episode",Json{{"event","end"},{"id","future"},{"t0",base+10},{"t1",base+3000}},base+100);
    assert(h.summary(base,base+6*H)["total_count"]==2);

    // Calm-room baseline: hours where the baby was in view most of the time and nobody cried.
    // Hour 0 has cries; hours 1-3 are calm; hour 4 the baby is away; hour 5 has a cry.
    int seq=1,rseq=0;
    const double temps[]={20,22,23,24,30,19};
    for(int hour=0;hour<6;++hour){
      h.observe(env(++seq,temps[hour],40+hour,700+hour),base+hour*H+100);
      for(int i=0;i<10;++i)
        event("sensor/radar",Json{{"boot_id","pi4"},{"seq",++rseq},{"valid",true},{"presence_valid",true},{"presence",hour!=4}},base+hour*H+200+i);
    }
    event("cry/episode",Json{{"event","end"},{"id","late"},{"t0",base+5*H+50},{"t1",base+5*H+90}},base+5*H+90);
    const auto quiet=h.summary(base,base+6*H)["quiet"];
    assert(quiet["count"]==3);assert(quiet["bucket_seconds"]==3600);
    assert(quiet["temperature_c"]["min"]==22);assert(quiet["temperature_c"]["max"]==24);
    assert(quiet["co2_ppm"]["min"]==701);assert(quiet["co2_ppm"]["max"]==703);

    {std::ofstream malformed(dir/"broken.json");malformed<<"not JSON";}
    assert(h.import_cry_log((dir/"broken.json").string())["ok"]==false);
    assert(h.summary(base,base+6*H)["total_count"]==3);
    std::ofstream file(dir/"cry_log.json");
    file<<Json{{"episodes",Json::array({Json{{"id","three"},{"t0",base+50},{"t1",base+80},{"action","hold"},{"answered_at",base+90}},
      Json{{"id","one"},{"t0",base+2},{"t1",base+4},{"action","diaper"},{"answered_at",0}}})}}.dump();file.close();
    assert(h.import_cry_log((dir/"cry_log.json").string())["ok"]==true);
    assert(h.import_cry_log((dir/"cry_log.json").string())["ok"]==true);
    summary=h.summary(base,base+6*H);assert(summary["total_count"]==4);
    const auto& first=summary["episodes"][3];assert(first["id"]=="one");assert(first["action"]=="feed");assert(first["t1"]==base+5);
    assert(first["room"]["temp"]==21.5); // import keeps the stored room state
    event("cry/feedback",Json{{"event","answer"},{"id","three"},{"ts",base+100},{"action","diaper"}},base+100);
    event("cry/feedback",Json{{"event","answer"},{"id","three"},{"ts",base+95},{"action","feed"}},base+101);
    assert(h.summary(base,base+6*H)["episodes"][1]["action"]=="diaper");
  }
  // Dedup keys are short hashes, not copies of the payload; per-minute tables are gone.
  assert(scalar(cfg.path,"SELECT max(length(event_key)) FROM recorded_events")<80);
  assert(scalar(cfg.path,"SELECT count(*) FROM sqlite_master WHERE name IN ('sensor_minutes','quiet_environment','environment_minutes')")==0);
  {
    HistoryStore h(cfg);assert(h.summary(base,base+6*H)["total_count"]==4);
    assert(h.summary(base,base+6*H)["quiet"]["count"]==3);
  }
  cfg.now=[=]{return double(base+40*86400);};
  {
    HistoryStore h(cfg);assert(h.summary(base,base+6*H)["total_count"]==0);
    assert(h.events(base,base+6*H)["records"].empty());
  }
  // Upgrading a schema-2 database folds per-minute rows into hours and drops the per-minute tables.
  cfg.path=(dir/"old.db").string();cfg.now=[=]{return double(base+2000);};sqlite3* db=nullptr;
  assert(sqlite3_open(cfg.path.c_str(),&db)==SQLITE_OK);
  const std::string old="CREATE TABLE environment_minutes(minute_s INTEGER PRIMARY KEY,total_count INTEGER NOT NULL,valid_count INTEGER NOT NULL,temp_sum REAL,temp_min REAL,temp_max REAL,humidity_sum REAL,humidity_min REAL,humidity_max REAL,co2_sum REAL,co2_min REAL,co2_max REAL);"
    "CREATE TABLE environment_sources(source TEXT PRIMARY KEY,sequence INTEGER NOT NULL);"
    "CREATE TABLE sensor_minutes(topic TEXT NOT NULL,minute_s INTEGER NOT NULL,stats TEXT NOT NULL,PRIMARY KEY(topic,minute_s));"
    "CREATE TABLE cry_episodes(id TEXT PRIMARY KEY,t0 REAL,t1 REAL,action TEXT,answered_at REAL NOT NULL DEFAULT 0,dropped INTEGER NOT NULL DEFAULT 0);"
    "PRAGMA user_version=2;INSERT INTO environment_minutes VALUES("+std::to_string(base)+",1,1,22,22,22,45,45,45,650,650,650);"
    "INSERT INTO environment_minutes VALUES("+std::to_string(base+60)+",1,1,24,24,24,47,47,47,700,700,700);"
    "INSERT INTO cry_episodes VALUES('kept',"+std::to_string(base+5)+","+std::to_string(base+9)+",'hold',0,0);";
  assert(sqlite3_exec(db,old.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(db);
  {HistoryStore h(cfg);const auto row=h.query(base,base+H)["records"][0];
   assert(row["valid_count"]==2);assert(row["temperature_c"]["mean"]==23);assert(row["temperature_c"]["max"]==24);
   const auto kept=h.summary(base,base+H)["episodes"][0];assert(kept["id"]=="kept");assert(kept["room"].is_null());}
  assert(scalar(cfg.path,"SELECT count(*) FROM sqlite_master WHERE name IN ('sensor_minutes','environment_minutes')")==0);
  assert(scalar(cfg.path,"PRAGMA user_version")==3);
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
   assert(h.query(base,base+H)["records"].size()==1);
  }
  assert(std::filesystem::file_size(cfg.path)<=4*1024*1024);
  std::filesystem::remove_all(dir);std::cout<<"history records: cry room state, change-only control events, calm-room baseline, JSON import, migration, restart and size cap OK\n";
}
