#include "history_records.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
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
bool yes(const Json& j,const char* key){return j.contains(key)&&j[key].is_boolean()&&j[key].get<bool>();}
std::string text(const Json& j,const char* key){return j.contains(key)&&j[key].is_string()?j[key].get<std::string>():"";}
bool time_value(const Json& j,const char* key,double& x){return number(j,key,x)&&x>=0&&x<253402300799.0;}
void metric(Json& j,const std::string& key,double value){
  if(!j.contains(key))j[key]=Json{{"count",0},{"sum",0.0},{"min",value},{"max",value}};
  auto& m=j[key];m["count"]=m["count"].get<int64_t>()+1;m["sum"]=m["sum"].get<double>()+value;
  m["min"]=std::min(m["min"].get<double>(),value);m["max"]=std::max(m["max"].get<double>(),value);
}
void increment(Json& j,const char* key,int n=1){j[key]=j.value(key,int64_t(0))+n;}
Json combine(Json a,const Json& b){
  if(!a.is_object())a=Json::object();
  for(auto it=b.begin();it!=b.end();++it){
    if(it.value().is_object()){
      if(!a.contains(it.key()))a[it.key()]=it.value();
      else {auto& dst=a[it.key()];const auto& src=it.value();dst["count"]=dst["count"].get<int64_t>()+src["count"].get<int64_t>();
        dst["sum"]=dst["sum"].get<double>()+src["sum"].get<double>();
        dst["min"]=std::min(dst["min"].get<double>(),src["min"].get<double>());dst["max"]=std::max(dst["max"].get<double>(),src["max"].get<double>());}
    }else a[it.key()]=a.value(it.key(),int64_t(0))+it.value().get<int64_t>();
  }
  return a;
}
bool valid_action(const std::string& a){for(const char* x:{"feed","diaper","hold","sleep","burp","temp","self","sick","other","unknown"})if(a==x)return true;return false;}
}
HistoryRecords::HistoryRecords(sqlite3* db):db_(db){
  receipt_nonce_=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  sql(db_,R"SQL(
CREATE TABLE IF NOT EXISTS history_metadata(key TEXT PRIMARY KEY,value REAL NOT NULL);
INSERT OR IGNORE INTO history_metadata VALUES('recorded_events_pruned_until',-1),('sensor_minutes_pruned_until',-1),
 ('environment_minutes_pruned_until',-1),('quiet_environment_pruned_until',-1),('cry_episodes_pruned_until',-1);
CREATE TABLE IF NOT EXISTS sensor_minutes(topic TEXT NOT NULL,minute_s INTEGER NOT NULL,stats TEXT NOT NULL,PRIMARY KEY(topic,minute_s));
CREATE INDEX IF NOT EXISTS sensor_minutes_time ON sensor_minutes(minute_s);
CREATE TABLE IF NOT EXISTS history_sources(source TEXT PRIMARY KEY,sequence INTEGER NOT NULL,seen_s REAL NOT NULL);
CREATE TABLE IF NOT EXISTS recorded_events(id INTEGER PRIMARY KEY AUTOINCREMENT,event_key TEXT NOT NULL UNIQUE,topic TEXT NOT NULL,payload TEXT NOT NULL,received_s REAL NOT NULL);
CREATE INDEX IF NOT EXISTS recorded_events_time ON recorded_events(received_s,id);
CREATE TABLE IF NOT EXISTS cry_episodes(id TEXT PRIMARY KEY,t0 REAL,t1 REAL,action TEXT,answered_at REAL NOT NULL DEFAULT 0,dropped INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS cry_episodes_time ON cry_episodes(t0);
CREATE TABLE IF NOT EXISTS quiet_environment(minute_s INTEGER PRIMARY KEY,stats TEXT NOT NULL);
)SQL");
  Stmt load(db_,"SELECT source,sequence,seen_s FROM history_sources");
  while(load.row())sources_[load.str(0)]={sqlite3_column_int64(load.p,1),sqlite3_column_double(load.p,2)};
}
bool HistoryRecords::fresh(const std::string& topic,const Json& j,double at){
  std::string source;int64_t seq=-1;
  // Radar sample_id describes the underlying breath, not the whole 1 Hz observation.
  // Its publication seq is needed to preserve presence/apnea transitions as well.
  if(j.contains("seq")&&j["seq"].is_number_integer()&&!text(j,"boot_id").empty()){
    source=topic+":"+text(j,"boot_id");seq=j["seq"].get<int64_t>();
  }else{double ts;if(!time_value(j,"ts",ts))return false;source=topic+":legacy";seq=static_cast<int64_t>(ts*1000);}
  if(source.size()>200||seq<0)return false;
  auto found=sources_.find(source);
  if(found!=sources_.end()&&seq<=found->second.first)return false;
  if(found==sources_.end()&&sources_.size()>=4096)throw std::runtime_error("history source limit reached");
  sources_[source]={seq,at};return true;
}
void HistoryRecords::ingest(const std::string& topic,const Json& j,double at){
  if(topic=="sensor/radar"||topic=="audio/cry"){
    if(!fresh(topic,j,at))return;
    const int64_t minute=static_cast<int64_t>(at)/60*60;
    if(!minutes_.count({topic,minute})&&minutes_.size()>=2880)throw std::runtime_error("pending sensor minute limit reached");
    auto& stats=minutes_[{topic,minute}];if(!stats.is_object())stats=Json::object();increment(stats,"sample_count");
    double ts;
    bool valid=(!j.contains("valid")||yes(j,"valid")) &&
      (text(j,"source")=="pi4-usb"||(time_value(j,"ts",ts)&&at-ts>=-2&&at-ts<=5));
    if(topic=="audio/cry"){
      double score;valid=valid&&j.contains("is_crying")&&j["is_crying"].is_boolean()&&number(j,"confidence",score)&&score>=0&&score<=1;
      if(valid){increment(stats,"valid_count");metric(stats,"confidence",score);if(yes(j,"is_crying"))increment(stats,"crying_count");}
      const bool continuous=last_audio_>=0&&at>=last_audio_&&at-last_audio_<=5;
      if(!valid||yes(j,"is_crying")){quiet_start_=-1;audio_quiet_=false;}
      else{if(!continuous||!audio_quiet_)quiet_start_=at;audio_quiet_=true;}
      last_audio_=at;
    }else if(valid){
      increment(stats,"valid_count");if(yes(j,"presence"))increment(stats,"presence_count");
      if(yes(j,"apnea"))increment(stats,"apnea_count");
      double v;if(yes(j,"presence")&&yes(j,"breath_valid")&&number(j,"breathing_rate",v)&&v>0&&v<=200)metric(stats,"breathing_rate",v);
      if(number(j,"movement",v)&&v>=0&&v<=1)metric(stats,"movement",v);
    }
    return;
  }
  if(topic.rfind("cry/",0)!=0&&topic!="fusion/alert"&&topic!="event/tof"&&topic.rfind("control/",0)!=0)return;
  // Canonical key order makes a local event and its MQTT echo idempotent.
  const std::string payload=nlohmann::json::parse(j.dump()).dump();
  if(payload.size()>32768)throw std::runtime_error("history event exceeds 32 KiB");
  double stamp;
  const bool identified=!text(j,"id").empty()||!text(j,"event_id").empty()||!text(j,"command_id").empty()||
    time_value(j,"ts",stamp)||time_value(j,"t0",stamp)||time_value(j,"t1",stamp)||
    (!text(j,"boot_id").empty()&&j.contains("seq")&&j["seq"].is_number_integer());
  const auto key=identified ? topic+":"+payload : "receipt:"+receipt_nonce_+":"+std::to_string(++receipt_seq_);
  if(!events_.count(key)&&events_.size()>=4096)throw std::runtime_error("pending event limit reached");
  events_.emplace(key,Event{topic,payload,at});
  if(topic=="cry/episode"||topic=="cry/feedback")episode(j,at);
}
void HistoryRecords::environment(const Json& j,double at){
  double age=0;number(j,"age_s",age);
  if(!audio_quiet_||quiet_start_<0||at<last_audio_||at-last_audio_>5||at-age-quiet_start_<600)return;
  const int64_t minute=static_cast<int64_t>(at)/60*60;
  if(!quiet_.count(minute)&&quiet_.size()>=1440)throw std::runtime_error("pending quiet minute limit reached");
  auto& stats=quiet_[minute];if(!stats.is_object())stats=Json::object();
  for(const char* name:{"temp","humidity","co2"})metric(stats,name,j[name].get<double>());
}
void HistoryRecords::episode(const Json& event,double at){
  const auto id=text(event,"id"),kind=text(event,"event");
  if(id.empty()||id.size()>160)return;
  if(kind=="start"){quiet_start_=-1;audio_quiet_=false;}
  if(kind!="start"&&kind!="end"&&kind!="drop"&&kind!="answer")return;
  double start,end;
  if(kind=="start"||kind=="end"||kind=="drop") {
    if(!time_value(event,"t0",start)||start>at+2)return;
    if(kind=="end"&&(!time_value(event,"t1",end)||end<start||end>at+2||end-start>31*86400.0))return;
  }
  if(!episodes_.count(id)){
    if(episodes_.size()>=10080)throw std::runtime_error("pending episode limit reached");
    Stmt load(db_,"SELECT t0,t1,action,answered_at,dropped FROM cry_episodes WHERE id=?");load.text(1,id);
    Json row{{"t0",nullptr},{"t1",nullptr},{"action",nullptr},{"answered_at",0.0},{"dropped",false}};
    if(load.row()){
      if(sqlite3_column_type(load.p,0)!=SQLITE_NULL)row["t0"]=sqlite3_column_double(load.p,0);
      if(sqlite3_column_type(load.p,1)!=SQLITE_NULL)row["t1"]=sqlite3_column_double(load.p,1);
      if(sqlite3_column_type(load.p,2)!=SQLITE_NULL)row["action"]=load.str(2);
      row["answered_at"]=sqlite3_column_double(load.p,3);row["dropped"]=sqlite3_column_int(load.p,4)!=0;
    }
    episodes_[id]=std::move(row);
  }
  auto& row=episodes_[id];double x;
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
  for(const auto& [key,value]:minutes_){
    if(key.second<cutoff)continue;
    Stmt previous(db_,"SELECT stats FROM sensor_minutes WHERE topic=? AND minute_s=?");previous.text(1,key.first);previous.integer(2,key.second);
    Json combined=previous.row()?combine(Json::parse(previous.str(0)),value):value;
    Stmt save(db_,"INSERT INTO sensor_minutes VALUES(?,?,?) ON CONFLICT(topic,minute_s) DO UPDATE SET stats=excluded.stats");
    save.text(1,key.first);save.integer(2,key.second);save.text(3,combined.dump());save.done();
  }
  for(const auto& [minute,value]:quiet_){
    if(minute<cutoff)continue;
    Stmt previous(db_,"SELECT stats FROM quiet_environment WHERE minute_s=?");previous.integer(1,minute);
    Json combined=previous.row()?combine(Json::parse(previous.str(0)),value):value;
    Stmt save(db_,"INSERT INTO quiet_environment VALUES(?,?) ON CONFLICT(minute_s) DO UPDATE SET stats=excluded.stats");
    save.integer(1,minute);save.text(2,combined.dump());save.done();
  }
  for(const auto& [id,row]:episodes_){
    if(row["t0"].is_number()&&row["t0"].get<double>()<cutoff)continue;
    Stmt save(db_,"INSERT INTO cry_episodes VALUES(?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET t0=excluded.t0,t1=excluded.t1,action=excluded.action,answered_at=excluded.answered_at,dropped=excluded.dropped");
    save.text(1,id);if(row["t0"].is_number())save.num(2,row["t0"].get<double>());
    if(row["t1"].is_number())save.num(3,row["t1"].get<double>());
    if(row["action"].is_string())save.text(4,row["action"].get<std::string>());
    save.num(5,row["answered_at"].get<double>());save.integer(6,row["dropped"].get<bool>());save.done();
  }
  written_events_.clear();
  for(const auto& [key,event]:events_){
    if(written_events_.size()>=16)break;
    written_events_.push_back(key);
    if(event.at<cutoff)continue;
    Stmt save(db_,"INSERT OR IGNORE INTO recorded_events(event_key,topic,payload,received_s) VALUES(?,?,?,?)");
    save.text(1,key);save.text(2,event.topic);save.text(3,event.payload);save.num(4,event.at);save.done();
  }
  for(auto it=sources_.begin();it!=sources_.end();){
    if(it->second.second<cutoff){it=sources_.erase(it);continue;}
    Stmt save(db_,"INSERT INTO history_sources VALUES(?,?,?) ON CONFLICT(source) DO UPDATE SET sequence=excluded.sequence,seen_s=excluded.seen_s");
    save.text(1,it->first);save.integer(2,it->second.first);save.num(3,it->second.second);save.done();++it;
  }
  for(const char* query:{"DELETE FROM sensor_minutes WHERE minute_s<?","DELETE FROM quiet_environment WHERE minute_s<?",
      "DELETE FROM recorded_events WHERE received_s<?","DELETE FROM cry_episodes WHERE coalesce(t0,answered_at)<?","DELETE FROM history_sources WHERE seen_s<?"}){
    Stmt prune(db_,query);prune.num(1,cutoff);prune.done();
  }
}
void HistoryRecords::committed(){minutes_.clear();quiet_.clear();episodes_.clear();for(const auto& key:written_events_)events_.erase(key);written_events_.clear();}
bool HistoryRecords::dirty()const{return !minutes_.empty()||!quiet_.empty()||!episodes_.empty()||!events_.empty();}
bool HistoryRecords::has_events()const{return !events_.empty()||!episodes_.empty();}
Json HistoryRecords::summary(int64_t from,int64_t to,uint32_t limit){
  Json hourly=Json::array();for(int i=0;i<24;++i)hourly.push_back(0);
  const char* filter="t0>=? AND t0<? AND t1>=t0 AND dropped=0";
  std::string counts="SELECT (CAST(t0 AS INTEGER)/3600+9)%24,count(*) FROM cry_episodes WHERE "+std::string(filter)+" GROUP BY 1";
  Stmt count(db_,counts.c_str());count.integer(1,from);count.integer(2,to);int64_t total=0;
  while(count.row()){const auto n=sqlite3_column_int64(count.p,1);hourly[sqlite3_column_int(count.p,0)]=n;total+=n;}
  std::string query="SELECT id,t0,t1,action FROM cry_episodes WHERE "+std::string(filter)+" ORDER BY t0 DESC,id LIMIT ?";
  Stmt rows(db_,query.c_str());rows.integer(1,from);rows.integer(2,to);rows.integer(3,limit);
  Json episodes=Json::array();
  while(rows.row())episodes.push_back(Json{{"id",rows.str(0)},{"t0",sqlite3_column_double(rows.p,1)},
    {"t1",sqlite3_column_double(rows.p,2)},{"action",sqlite3_column_type(rows.p,3)==SQLITE_NULL?Json(nullptr):Json(rows.str(3))}});
  Stmt quiet(db_,"SELECT stats FROM quiet_environment WHERE minute_s>=? AND minute_s<? ORDER BY minute_s");quiet.integer(1,from);quiet.integer(2,to);
  Json range=Json::object();int n=0;
  while(quiet.row()){range=combine(std::move(range),Json::parse(quiet.str(0)));++n;}
  Json quiet_result{{"count",n},{"temperature_c",nullptr},{"humidity_pct",nullptr},{"co2_ppm",nullptr}};
  if(n>=3)for(const auto& pair:{std::make_pair("temp","temperature_c"),std::make_pair("humidity","humidity_pct"),std::make_pair("co2","co2_ppm")}){
    const auto& m=range[pair.first];quiet_result[pair.second]=Json{{"min",m["min"]},{"max",m["max"]}};
  }
  return Json{{"ok",true},{"time_unit","unix_seconds"},{"timezone","Asia/Seoul"},{"from_s",from},{"to_s",to},
    {"limited_by_size",limited("cry_episodes",from)||limited("quiet_environment",from)},{"total_count",total},{"hourly",hourly},{"episodes",episodes},{"truncated",total>static_cast<int64_t>(episodes.size())},{"quiet",quiet_result}};
}
Json HistoryRecords::sensors(int64_t from,int64_t to,uint32_t limit){
  // Page whole minutes, never split radar and audio rows at the same timestamp.
  Stmt s(db_,"SELECT topic,minute_s,stats FROM sensor_minutes WHERE minute_s IN (SELECT DISTINCT minute_s FROM sensor_minutes WHERE minute_s>=? AND minute_s<? ORDER BY minute_s LIMIT ?) ORDER BY minute_s,topic");
  s.integer(1,from);s.integer(2,to);s.integer(3,int64_t(limit)+1);Json rows=Json::array();
  int64_t last=-1;uint32_t minutes=0;bool truncated=false;
  while(s.row()){
    const auto minute=sqlite3_column_int64(s.p,1);
    if(minute!=last){if(minutes==limit){truncated=true;break;}++minutes;last=minute;}
    rows.push_back(Json{{"topic",s.str(0)},{"minute_s",minute},{"stats",Json::parse(s.str(2))}});
  }
  return Json{{"ok",true},{"time_unit","unix_seconds"},{"limit_unit","minutes"},{"records",rows},
    {"truncated",truncated},{"next_from_s",truncated?last+60:to},{"limited_by_size",limited("sensor_minutes",from)}};
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
  // Raw control heartbeats can be large: evict these first, protecting summaries
  // and parent answers until lower-priority data is exhausted.
  for(const auto& table:{Table{"recorded_events","id","received_s"},Table{"sensor_minutes","rowid","minute_s"},
      Table{"environment_minutes","minute_s","minute_s"},Table{"quiet_environment","minute_s","minute_s"},
      Table{"cry_episodes","id","coalesce(t0,answered_at)"}}){
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
