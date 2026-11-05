#include "history_records.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
namespace nuni {
namespace {
struct Stmt {
  sqlite3* db; sqlite3_stmt* p=nullptr;
  Stmt(sqlite3* d,const char* sql):db(d){if(sqlite3_prepare_v2(d,sql,-1,&p,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(d));}
  ~Stmt(){sqlite3_finalize(p);}
  void num(int i,double x){sqlite3_bind_double(p,i,x);}
  void integer(int i,int64_t x){sqlite3_bind_int64(p,i,x);}
  void text(int i,const std::string& x){sqlite3_bind_text(p,i,x.c_str(),-1,SQLITE_TRANSIENT);}
  bool row(){const int rc=sqlite3_step(p);if(rc!=SQLITE_ROW&&rc!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));return rc==SQLITE_ROW;}
  void done(){if(row())throw std::runtime_error("unexpected SQL row");}
  std::string str(int i){const auto* x=sqlite3_column_text(p,i);return x?reinterpret_cast<const char*>(x):"";}
};
void sql(sqlite3* db,const char* text){char* err=nullptr;if(sqlite3_exec(db,text,nullptr,nullptr,&err)!=SQLITE_OK){std::string reason=err?err:sqlite3_errmsg(db);sqlite3_free(err);throw std::runtime_error(reason);}}
bool number(const Json& j,const char* key,double& out){if(!j.contains(key)||!j[key].is_number())return false;out=j[key].get<double>();return std::isfinite(out);}
std::string text(const Json& j,const char* key){return j.contains(key)&&j[key].is_string()?j[key].get<std::string>():"";}
bool time_value(const Json& j,const char* key,double& x){return number(j,key,x)&&x>=0&&x<253402300799.0;}
bool valid_action(const std::string& a){for(const char* x:{"feed","diaper","hold","sleep","burp","temp","self","sick","other","unknown"})if(a==x)return true;return false;}
// Stable across builds and restarts (unlike std::hash), so dedup keys survive upgrades.
std::string digest(const std::string& s){
  uint64_t h=1469598103934665603ULL;
  for(unsigned char c:s){h^=c;h*=1099511628211ULL;}
  char out[17];std::snprintf(out,sizeof(out),"%016llx",static_cast<unsigned long long>(h));return out;
}
// Published every 1-2 s; only a change of state is worth keeping.
const std::set<std::string> kPeriodic{"control/action","control/humidifier/command","control/humidifier/state",
  "control/humidifier/policy"};
// Fields that differ on every publication even when nothing changed.
const char* const kVolatile[]{"id","ts","seq","boot_id","issued_at","expires_at","command_id","sample_id",
  "uptime_ms","relay_seq"};
bool has_column(sqlite3* db,const char* table,const char* column){
  const std::string q=std::string("PRAGMA table_info(")+table+")";Stmt s(db,q.c_str());
  while(s.row())if(s.str(1)==column)return true;
  return false;
}
}
HistoryRecords::HistoryRecords(sqlite3* db,int bucket_s):db_(db),bucket_s_(bucket_s){
  receipt_nonce_=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  sql(db_,R"SQL(
CREATE TABLE IF NOT EXISTS history_metadata(key TEXT PRIMARY KEY,value REAL NOT NULL);
INSERT OR IGNORE INTO history_metadata VALUES('recorded_events_pruned_until',-1),
 ('environment_buckets_pruned_until',-1),('cry_episodes_pruned_until',-1);
CREATE TABLE IF NOT EXISTS recorded_events(id INTEGER PRIMARY KEY AUTOINCREMENT,event_key TEXT NOT NULL UNIQUE,topic TEXT NOT NULL,payload TEXT NOT NULL,received_s REAL NOT NULL);
CREATE INDEX IF NOT EXISTS recorded_events_time ON recorded_events(received_s,id);
CREATE TABLE IF NOT EXISTS cry_episodes(id TEXT PRIMARY KEY,t0 REAL,t1 REAL,action TEXT,answered_at REAL NOT NULL DEFAULT 0,dropped INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS cry_episodes_time ON cry_episodes(t0);
DROP TABLE IF EXISTS sensor_minutes;
DROP TABLE IF EXISTS quiet_environment;
DROP TABLE IF EXISTS history_sources;
DELETE FROM history_metadata WHERE key IN ('sensor_minutes_pruned_until','quiet_environment_pruned_until','environment_minutes_pruned_until');
)SQL");
  // Room state when the cry started (NULL when no fresh reading was available).
  for(const char* column:{"room_temp","room_humidity","room_co2"})
    if(!has_column(db_,"cry_episodes",column)){
      const std::string q=std::string("ALTER TABLE cry_episodes ADD COLUMN ")+column+" REAL";sql(db_,q.c_str());
    }
}
void HistoryRecords::ingest(const std::string& topic,const Json& j,double at){
  if(topic.rfind("cry/",0)!=0&&topic!="fusion/alert"&&topic!="event/tof"&&topic.rfind("control/",0)!=0)return;
  if(kPeriodic.count(topic)){
    Json state=j;for(const char* key:kVolatile)state.erase(key);
    auto signature=state.dump();
    auto& last=last_state_[topic];
    if(last==signature)return;
    last=std::move(signature);
  }
  // Canonical key order makes a local event and its MQTT echo idempotent.
  const std::string payload=nlohmann::json::parse(j.dump()).dump();
  if(payload.size()>32768)throw std::runtime_error("history event exceeds 32 KiB");
  double stamp;
  const bool identified=!text(j,"id").empty()||!text(j,"event_id").empty()||!text(j,"command_id").empty()||
    time_value(j,"ts",stamp)||time_value(j,"t0",stamp)||time_value(j,"t1",stamp)||
    (!text(j,"boot_id").empty()&&j.contains("seq")&&j["seq"].is_number_integer());
  const auto key=identified ? topic+":"+digest(payload) : "receipt:"+receipt_nonce_+":"+std::to_string(++receipt_seq_);
  if(!events_.count(key)&&events_.size()>=4096)throw std::runtime_error("pending event limit reached");
  events_.emplace(key,Event{topic,payload,at});
  if(topic=="cry/episode"||topic=="cry/feedback")episode(j,at);
}
void HistoryRecords::environment(const Json& j,double at){
  double t,h,c;
  if(number(j,"temp",t)&&number(j,"humidity",h)&&number(j,"co2",c))room_=Room{t,h,c,at};
}
void HistoryRecords::episode(const Json& event,double at){
  const auto id=text(event,"id"),kind=text(event,"event");
  if(id.empty()||id.size()>160)return;
  if(kind!="start"&&kind!="end"&&kind!="drop"&&kind!="answer")return;
  double start,end;
  if(kind=="start"||kind=="end"||kind=="drop") {
    if(!time_value(event,"t0",start)||start>at+2)return;
    if(kind=="end"&&(!time_value(event,"t1",end)||end<start||end>at+2||end-start>31*86400.0))return;
  }
  if(!episodes_.count(id)){
    if(episodes_.size()>=10080)throw std::runtime_error("pending episode limit reached");
    Stmt load(db_,"SELECT t0,t1,action,answered_at,dropped,room_temp,room_humidity,room_co2 FROM cry_episodes WHERE id=?");load.text(1,id);
    Json row{{"t0",nullptr},{"t1",nullptr},{"action",nullptr},{"answered_at",0.0},{"dropped",false},{"room",nullptr}};
    if(load.row()){
      if(sqlite3_column_type(load.p,0)!=SQLITE_NULL)row["t0"]=sqlite3_column_double(load.p,0);
      if(sqlite3_column_type(load.p,1)!=SQLITE_NULL)row["t1"]=sqlite3_column_double(load.p,1);
      if(sqlite3_column_type(load.p,2)!=SQLITE_NULL)row["action"]=load.str(2);
      row["answered_at"]=sqlite3_column_double(load.p,3);row["dropped"]=sqlite3_column_int(load.p,4)!=0;
      if(sqlite3_column_type(load.p,5)!=SQLITE_NULL)
        row["room"]=Json{{"temp",sqlite3_column_double(load.p,5)},{"humidity",sqlite3_column_double(load.p,6)},
                         {"co2",sqlite3_column_double(load.p,7)}};
    }
    episodes_[id]=std::move(row);
  }
  auto& row=episodes_[id];double x;
  // Room state at the start of the cry: the latest reading, if it is at most 2 minutes old.
  if(kind=="start"&&row["room"].is_null()&&room_.at>=0&&std::fabs(at-room_.at)<=120)
    row["room"]=Json{{"temp",room_.temp},{"humidity",room_.humidity},{"co2",room_.co2}};
  if(time_value(event,"t0",x)&&row["t0"].is_null())row["t0"]=x;
  if(kind=="end"&&time_value(event,"t1",x)&&(!row["t0"].is_number()||x>=row["t0"].get<double>())){
    if(row["t1"].is_null()||x>=row["t1"].get<double>())row["t1"]=x;
    row["dropped"]=false;
  }
  if(kind=="drop")row["dropped"]=true;
  const auto action=text(event,"action");
  double answered=0;
  if(kind=="answer"){if(!time_value(event,"ts",answered))answered=at;}
  else time_value(event,"answered_at",answered);
  if(answered>at+2)return;
  if(valid_action(action)&&answered>=row["answered_at"].get<double>()){
    row["action"]=action;row["answered_at"]=answered;
  }
}
void HistoryRecords::write(double cutoff){
  for(const auto& [id,row]:episodes_){
    if(row["t0"].is_number()&&row["t0"].get<double>()<cutoff)continue;
    Stmt save(db_,"INSERT INTO cry_episodes(id,t0,t1,action,answered_at,dropped,room_temp,room_humidity,room_co2) VALUES(?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(id) DO UPDATE SET t0=excluded.t0,t1=excluded.t1,action=excluded.action,answered_at=excluded.answered_at,"
      "dropped=excluded.dropped,room_temp=excluded.room_temp,room_humidity=excluded.room_humidity,room_co2=excluded.room_co2");
    save.text(1,id);if(row["t0"].is_number())save.num(2,row["t0"].get<double>());
    if(row["t1"].is_number())save.num(3,row["t1"].get<double>());
    if(row["action"].is_string())save.text(4,row["action"].get<std::string>());
    save.num(5,row["answered_at"].get<double>());save.integer(6,row["dropped"].get<bool>());
    if(row["room"].is_object()){
      save.num(7,row["room"]["temp"].get<double>());save.num(8,row["room"]["humidity"].get<double>());
      save.num(9,row["room"]["co2"].get<double>());
    }
    save.done();
  }
  written_events_.clear();
  for(const auto& [key,event]:events_){
    if(written_events_.size()>=64)break;
    written_events_.push_back(key);
    if(event.at<cutoff)continue;
    Stmt save(db_,"INSERT OR IGNORE INTO recorded_events(event_key,topic,payload,received_s) VALUES(?,?,?,?)");
    save.text(1,key);save.text(2,event.topic);save.text(3,event.payload);save.num(4,event.at);save.done();
  }
  for(const char* query:{"DELETE FROM recorded_events WHERE received_s<?","DELETE FROM cry_episodes WHERE coalesce(t0,answered_at)<?"}){
    Stmt prune(db_,query);prune.num(1,cutoff);prune.done();
  }
}
void HistoryRecords::committed(){episodes_.clear();for(const auto& key:written_events_)events_.erase(key);written_events_.clear();}
bool HistoryRecords::dirty()const{return !episodes_.empty()||!events_.empty();}
bool HistoryRecords::has_events()const{return !events_.empty()||!episodes_.empty();}
Json HistoryRecords::summary(int64_t from,int64_t to,uint32_t limit){
  Json hourly=Json::array();for(int i=0;i<24;++i)hourly.push_back(0);
  const char* filter="t0>=? AND t0<? AND t1>=t0 AND dropped=0";
  std::string counts="SELECT (CAST(t0 AS INTEGER)/3600+9)%24,count(*) FROM cry_episodes WHERE "+std::string(filter)+" GROUP BY 1";
  Stmt count(db_,counts.c_str());count.integer(1,from);count.integer(2,to);int64_t total=0;
  while(count.row()){const auto n=sqlite3_column_int64(count.p,1);hourly[sqlite3_column_int(count.p,0)]=n;total+=n;}
  std::string query="SELECT id,t0,t1,action,room_temp,room_humidity,room_co2 FROM cry_episodes WHERE "+std::string(filter)+" ORDER BY t0 DESC,id LIMIT ?";
  Stmt rows(db_,query.c_str());rows.integer(1,from);rows.integer(2,to);rows.integer(3,limit);
  Json episodes=Json::array();
  while(rows.row()){
    Json room=nullptr;
    if(sqlite3_column_type(rows.p,4)!=SQLITE_NULL)
      room=Json{{"temp",sqlite3_column_double(rows.p,4)},{"humidity",sqlite3_column_double(rows.p,5)},{"co2",sqlite3_column_double(rows.p,6)}};
    episodes.push_back(Json{{"id",rows.str(0)},{"t0",sqlite3_column_double(rows.p,1)},
      {"t1",sqlite3_column_double(rows.p,2)},{"action",sqlite3_column_type(rows.p,3)==SQLITE_NULL?Json(nullptr):Json(rows.str(3))},
      {"room",room}});
  }
  // Calm-room baseline: summaries where the radar saw the baby most of the time and no cry overlapped.
  Stmt calm(db_,"SELECT min(temp_min),max(temp_max),min(humidity_min),max(humidity_max),min(co2_min),max(co2_max),count(*) "
    "FROM environment_buckets e WHERE bucket_s>=? AND bucket_s<? AND valid_count>0 AND radar_count>0 AND presence_count*2>=radar_count "
    "AND NOT EXISTS(SELECT 1 FROM cry_episodes c WHERE c.dropped=0 AND c.t0<e.bucket_s+? AND coalesce(c.t1,c.t0)>=e.bucket_s)");
  calm.integer(1,from);calm.integer(2,to);calm.integer(3,bucket_s_);calm.row();
  const int n=sqlite3_column_int(calm.p,6);
  Json quiet_result{{"count",n},{"bucket_seconds",bucket_s_},{"temperature_c",nullptr},{"humidity_pct",nullptr},{"co2_ppm",nullptr}};
  if(n>=3){
    int i=0;
    for(const char* name:{"temperature_c","humidity_pct","co2_ppm"}){
      quiet_result[name]=Json{{"min",sqlite3_column_double(calm.p,i)},{"max",sqlite3_column_double(calm.p,i+1)}};i+=2;
    }
  }
  return Json{{"ok",true},{"time_unit","unix_seconds"},{"timezone","Asia/Seoul"},{"from_s",from},{"to_s",to},
    {"limited_by_size",limited("cry_episodes",from)||limited("environment_buckets",from)},{"total_count",total},{"hourly",hourly},{"episodes",episodes},{"truncated",total>static_cast<int64_t>(episodes.size())},{"quiet",quiet_result}};
}
Json HistoryRecords::events(int64_t from,int64_t to,uint32_t limit,int64_t after){
  Stmt s(db_,"SELECT id,topic,payload,received_s FROM recorded_events WHERE received_s>=? AND received_s<? AND id>? ORDER BY id LIMIT ?");
  s.integer(1,from);s.integer(2,to);s.integer(3,after);s.integer(4,int64_t(limit)+1);Json rows=Json::array();
  while(s.row())rows.push_back(Json{{"id",sqlite3_column_int64(s.p,0)},{"topic",s.str(1)},{"payload",Json::parse(s.str(2))},{"received_s",sqlite3_column_double(s.p,3)}});
  const bool truncated=rows.size()>limit;if(truncated)rows.erase(rows.end()-1);
  const int64_t next=rows.empty()?after:rows.back()["id"].get<int64_t>();
  return Json{{"ok",true},{"time_unit","unix_seconds"},{"records",rows},{"truncated",truncated},{"after_id",next},{"limited_by_size",limited("recorded_events",from)}};
}
size_t HistoryRecords::import_file(const std::string& path,double now,int days){
  std::error_code error;
  const bool exists=std::filesystem::exists(path,error);
  if(error)throw std::runtime_error("cannot inspect cry JSON import");
  if(!exists)return 0;
  const auto size=std::filesystem::file_size(path,error);
  if(error||size>32*1024*1024)throw std::runtime_error("cry JSON import exceeds 32 MiB or is unreadable");
  std::ifstream f(path);if(!f)throw std::runtime_error("cannot read cry JSON import");
  Json root=Json::parse(f);if(!root.is_object()||!root.contains("episodes")||!root["episodes"].is_array())throw std::runtime_error("invalid cry JSON import");
  std::vector<Json> staged;
  for(auto row:root["episodes"]){
    double t0,t1;if(!row.is_object()||!time_value(row,"t0",t0)||!time_value(row,"t1",t1)||t0<now-days*86400.0||t0>now||t1<t0||t1>now||text(row,"id").empty()||text(row,"id").size()>160||t1-t0>31*86400.0)continue;
    if(staged.size()>=10080)throw std::runtime_error("cry JSON import exceeds 10080 recent episodes");
    row["event"]="end";staged.push_back(std::move(row));
  }
  for(const auto& row:staged)episode(row,now);
  return staged.size();
}
bool HistoryRecords::limited(const std::string& table,int64_t from){
  Stmt s(db_,"SELECT value FROM history_metadata WHERE key=?");s.text(1,table+"_pruned_until");
  return s.row()&&sqlite3_column_double(s.p,0)>=from;
}
void HistoryRecords::make_room(bool force){
  const auto used=[&]{
    Stmt pages(db_,"PRAGMA page_count");pages.row();const auto count=sqlite3_column_int64(pages.p,0);
    Stmt free(db_,"PRAGMA freelist_count");free.row();return count-sqlite3_column_int64(free.p,0);
  };
  int64_t cap;
  {Stmt max(db_,"PRAGMA max_page_count");max.row();cap=sqlite3_column_int64(max.p,0);}
  if(!force&&used()<cap*8/10)return;
  struct Table {const char* name;const char* key;const char* time;};
  // Evict raw events first, protecting room summaries and parent answers until events are exhausted.
  for(const auto& table:{Table{"recorded_events","id","received_s"},
      Table{"environment_buckets","bucket_s","bucket_s"},Table{"cry_episodes","id","coalesce(t0,answered_at)"}}){
    for(int pass=0;pass<64&&used()>=cap*65/100;++pass){
      const auto where=std::string(table.key)+" IN (SELECT "+table.key+" FROM "+table.name+" ORDER BY "+table.time+
        " LIMIT max(1,(SELECT count(*)/5 FROM "+table.name+")))";
      double until=-1;
      {const auto query="SELECT max("+std::string(table.time)+") FROM "+table.name+" WHERE "+where;
       Stmt s(db_,query.c_str());s.row();if(sqlite3_column_type(s.p,0)!=SQLITE_NULL)until=sqlite3_column_double(s.p,0);}
      const auto query="DELETE FROM "+std::string(table.name)+" WHERE "+where;sql(db_,query.c_str());
      if(sqlite3_changes(db_)==0)break;
      Stmt note(db_,"UPDATE history_metadata SET value=max(value,?) WHERE key=?");
      note.num(1,until);note.text(2,std::string(table.name)+"_pruned_until");note.done();
    }
    if(used()<cap*65/100)break;
  }
}
}
