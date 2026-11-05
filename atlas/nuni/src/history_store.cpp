#include "nuni/history_store.hpp"
#include <sqlite3.h>
#include "history_records.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace nuni {
namespace {
constexpr int64_t kMaxTime = 253402300799LL;
constexpr int kSchemaVersion = 3;
struct Statement {
  sqlite3_stmt* p = nullptr;
  Statement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &p, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(db));
  }
  ~Statement() { sqlite3_finalize(p); }
  void integer(int i, int64_t x) { sqlite3_bind_int64(p, i, x); }
  void real(int i, double x) { sqlite3_bind_double(p, i, x); }
  void text(int i, const std::string& x) { sqlite3_bind_text(p, i, x.c_str(), -1, SQLITE_TRANSIENT); }
  void done(sqlite3* db) {
    if (sqlite3_step(p) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
  }
};
void exec(sqlite3* db, const std::string& sql) {
  char* error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    std::string reason = error ? error : sqlite3_errmsg(db);
    sqlite3_free(error);
    throw std::runtime_error(reason);
  }
}
bool table_exists(sqlite3* db, const char* name) {
  Statement s(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
  s.text(1, name);
  return sqlite3_step(s.p) == SQLITE_ROW;
}
std::string bucket_table(const char* name) {
  return std::string("CREATE TABLE IF NOT EXISTS ") + name + R"SQL((
 bucket_s INTEGER PRIMARY KEY, total_count INTEGER NOT NULL, valid_count INTEGER NOT NULL,
 temp_sum REAL, temp_min REAL, temp_max REAL,
 humidity_sum REAL, humidity_min REAL, humidity_max REAL,
 co2_sum REAL, co2_min REAL, co2_max REAL,
 radar_count INTEGER NOT NULL DEFAULT 0, presence_count INTEGER NOT NULL DEFAULT 0);)SQL";
}
Json failure(const std::string& reason) { return Json{{"ok", false}, {"error", reason}}; }
bool numeric(const Json& j, const char* key, double& x) {
  if (!j.contains(key) || !j[key].is_number()) return false;
  x = j[key].get<double>();
  return std::isfinite(x);
}
struct Metric {
  double sum = 0, lo = 0, hi = 0;
  void add(double x, bool first) { sum += x; lo = first ? x : std::min(lo,x); hi = first ? x : std::max(hi,x); }
};
// One room-state summary: environment readings plus how often the radar saw the baby.
struct Bucket { int total = 0, valid = 0, radar = 0, presence = 0; Metric t,h,c; };
}
struct HistoryStore::Impl {
  Config cfg;
  sqlite3* db = nullptr;
  struct Work {
    Json sample;
    std::string topic="sensor/env",kind;
    int64_t after=0;
    double at = 0;
    int64_t from = 0, to = 0;
    uint32_t limit = 0;
    std::shared_ptr<std::promise<Json>> reply;
  };
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Work> queue;
  bool stopping = false;
  uint64_t dropped = 0;
  std::thread worker;
  std::map<int64_t,Bucket> buckets;
  // Device namespace -> last sequence. Persisted with the summaries.
  std::map<std::string,int64_t> sources;
  std::map<std::string,double> source_seen;
  // Radar publication identity (boot_id -> seq); only used to count each publication once.
  std::map<std::string,int64_t> radar_seen;
  std::string last_error;
  std::unique_ptr<HistoryRecords> records;
  bool disk_full=false;

  explicit Impl(Config c) : cfg(std::move(c)) {
    if (cfg.retention_days < 7 || cfg.retention_days > 365 || cfg.max_mib < 4 || cfg.max_mib > 1024)
      throw std::invalid_argument("history retention must be 7..365 days; size 4..1024 MiB");
    if (cfg.bucket_s != 3600 && cfg.bucket_s != 7200)
      throw std::invalid_argument("history room-state bucket must be 3600 or 7200 seconds");
    if (cfg.flush_s < 1 || cfg.flush_s > 600) throw std::invalid_argument("history flush interval must be 1..600 seconds");
    if (!cfg.now) cfg.now = [] {
      return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    };
    const auto parent = std::filesystem::path(cfg.path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    if (sqlite3_open_v2(cfg.path.c_str(), &db, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
      std::string reason = db ? sqlite3_errmsg(db) : "cannot allocate SQLite handle";
      if (db) sqlite3_close(db);
      db = nullptr;
      throw std::runtime_error(reason);
    }
    try {
      sqlite3_busy_timeout(db, 1000);
      // WAL + NORMAL cannot corrupt the database; a power cut loses at most the last commit.
      exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=256; PRAGMA journal_size_limit=1048576;");
      // Statements are scoped so none is still open when tables are dropped below (SQLITE_LOCKED).
      { Statement version(db,"PRAGMA user_version");
        if (sqlite3_step(version.p) != SQLITE_ROW || sqlite3_column_int(version.p,0) > kSchemaVersion)
          throw std::runtime_error("unsupported history schema version"); }
      int page_size=0;
      { Statement page(db,"PRAGMA page_size");
        if (sqlite3_step(page.p) != SQLITE_ROW) throw std::runtime_error("cannot read database page size");
        page_size = sqlite3_column_int(page.p,0); }
      exec(db,"PRAGMA max_page_count="+std::to_string(int64_t(cfg.max_mib)*1024*1024/page_size));
      exec(db,"BEGIN IMMEDIATE");
      exec(db, bucket_table("environment_buckets"));
      exec(db, R"SQL(
CREATE TABLE IF NOT EXISTS environment_sources(source TEXT PRIMARY KEY, sequence INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS history_metadata(key TEXT PRIMARY KEY,value REAL NOT NULL);
)SQL");
      const auto bucket=std::to_string(cfg.bucket_s);
      const auto sums="sum(total_count),sum(valid_count),sum(temp_sum),min(temp_min),max(temp_max),"
        "sum(humidity_sum),min(humidity_min),max(humidity_max),sum(co2_sum),min(co2_min),max(co2_max)";
      // Schema 1-2 kept one row per minute; fold them into room-state buckets.
      if (table_exists(db,"environment_minutes")) {
        exec(db,"INSERT INTO environment_buckets SELECT minute_s/"+bucket+"*"+bucket+","+sums+",0,0 FROM environment_minutes "
          "WHERE 1 GROUP BY 1 ON CONFLICT(bucket_s) DO NOTHING; DROP TABLE environment_minutes;");
      }
      // Re-bucket when the configured length changes (1 h <-> 2 h).
      int stored_bucket=0;
      { Statement stored(db,"SELECT value FROM history_metadata WHERE key='environment_bucket_s'");
        if (sqlite3_step(stored.p)==SQLITE_ROW) stored_bucket=sqlite3_column_int(stored.p,0); }
      if (stored_bucket && stored_bucket!=cfg.bucket_s) {
        exec(db,bucket_table("environment_buckets_new"));
        exec(db,"INSERT INTO environment_buckets_new SELECT bucket_s/"+bucket+"*"+bucket+","+sums+
          ",sum(radar_count),sum(presence_count) FROM environment_buckets WHERE 1 GROUP BY 1; "
          "DROP TABLE environment_buckets; ALTER TABLE environment_buckets_new RENAME TO environment_buckets;");
      }
      exec(db,"INSERT INTO history_metadata VALUES('environment_bucket_s',"+bucket+") ON CONFLICT(key) DO UPDATE SET value=excluded.value");
      bool seen_column=false;
      { Statement columns(db,"PRAGMA table_info(environment_sources)");
        while(sqlite3_step(columns.p)==SQLITE_ROW) {
          const auto* name=reinterpret_cast<const char*>(sqlite3_column_text(columns.p,1));
          if(name && std::string(name)=="seen_s")seen_column=true;
        }
      }
      if(!seen_column)exec(db,"ALTER TABLE environment_sources ADD COLUMN seen_s REAL NOT NULL DEFAULT 0");
      { Statement migrate(db,"UPDATE environment_sources SET seen_s=? WHERE seen_s=0");
        migrate.real(1,cfg.now());migrate.done(db); }
      { Statement load(db,"SELECT source,sequence,seen_s FROM environment_sources");
        while (sqlite3_step(load.p)==SQLITE_ROW) {
          const auto source=std::string(reinterpret_cast<const char*>(sqlite3_column_text(load.p,0)));
          sources[source]=sqlite3_column_int64(load.p,1);source_seen[source]=sqlite3_column_double(load.p,2);
        } }
      records=std::make_unique<HistoryRecords>(db,cfg.bucket_s);
      exec(db,"PRAGMA user_version="+std::to_string(kSchemaVersion)+"; COMMIT");
      worker = std::thread([this]{run();});
    } catch (...) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); sqlite3_close(db); db=nullptr; throw; }
  }
  ~Impl() {
    { std::lock_guard<std::mutex> lock(mutex); stopping=true; }
    wake.notify_one();
    worker.join();
    sqlite3_close(db);
  }
  int64_t bucket_of(double at) const { return static_cast<int64_t>(at)/cfg.bucket_s*cfg.bucket_s; }
  bool open_bucket(int64_t key) {
    // Bounded memory even when the disk is full or the system clock jumps (31 days of hourly buckets).
    if (!buckets.count(key) && buckets.size()>=744) { last_error="history pending bucket limit reached"; return false; }
    return true;
  }
  // Prefer ESP measurement identity, not the hub's once-per-second publication sequence.
  bool fresh(const Json& j,double at) {
    std::string source;
    int64_t seq = -1;
    if (j.contains("sample_id") && j["sample_id"].is_string()) {
      auto id = j["sample_id"].get<std::string>();
      const auto pos = id.rfind(':');
      if (id.size()>160 || pos==std::string::npos || pos==0) return false;
      source="sample:"+id.substr(0,pos);
      try {
        size_t used=0;
        auto suffix=id.substr(pos+1);
        seq=std::stoll(suffix,&used);
        if (used!=suffix.size()) return false;
      } catch (...) { return false; }
    } else if (j.contains("boot_id") && j["boot_id"].is_string() && j.contains("seq") && j["seq"].is_number_integer()) {
      source="boot:"+j["boot_id"].get<std::string>();
      seq=j["seq"].get<int64_t>();
    } else {
      double ts;
      if (!numeric(j,"ts",ts) || ts<0 || ts>kMaxTime) return false;
      source="legacy:env"; seq=static_cast<int64_t>(ts*1000);
    }
    if (source.size()>170 || seq<0) return false;
    auto it=sources.find(source);
    if (it!=sources.end() && seq<=it->second) return false;
    if (it==sources.end() && sources.size()>=4096) throw std::runtime_error("environment source limit reached");
    sources[source]=seq;source_seen[source]=at;
    return true;
  }
  void ingest(const Work& w) {
    const auto key = bucket_of(w.at);
    if (!open_bucket(key)) return;
    if (!fresh(w.sample,w.at)) return;
    auto& b=buckets[key]; ++b.total;
    double t,h,c,age;
    bool valid = (!w.sample.contains("valid") || (w.sample["valid"].is_boolean() && w.sample["valid"].get<bool>()));
    if (w.sample.contains("age_s")) valid = valid && numeric(w.sample,"age_s",age) && age>=0 && age<=20;
    valid = valid && numeric(w.sample,"temp",t) && numeric(w.sample,"humidity",h) && numeric(w.sample,"co2",c);
    valid = valid && t>=-40 && t<=125 && h>=0 && h<=100 && c>=0 && c<=100000;
    if (w.sample.value("source",std::string())!="pi4-usb") {
      double ts;
      valid = valid && numeric(w.sample,"ts",ts) && w.at-ts>=-2 && w.at-ts<=20;
    }
    if (valid) { b.t.add(t,b.valid==0); b.h.add(h,b.valid==0); b.c.add(c,b.valid==0); ++b.valid; records->environment(w.sample,w.at); }
  }
  // Radar publications only count how much of the bucket the baby was in view (no per-minute statistics).
  void radar(const Work& w) {
    const auto& j=w.sample;
    if (!j.contains("boot_id") || !j["boot_id"].is_string() || !j.contains("seq") || !j["seq"].is_number_integer()) return;
    const auto boot=j["boot_id"].get<std::string>();const auto seq=j["seq"].get<int64_t>();
    if (boot.empty() || boot.size()>160 || seq<0) return;
    auto it=radar_seen.find(boot);
    if (it!=radar_seen.end() && seq<=it->second) return;
    if (it==radar_seen.end() && radar_seen.size()>=64) radar_seen.erase(radar_seen.begin());
    radar_seen[boot]=seq;
    const bool usable=(!j.contains("presence_valid") || (j["presence_valid"].is_boolean() && j["presence_valid"].get<bool>())) &&
                      (!j.contains("valid") || (j["valid"].is_boolean() && j["valid"].get<bool>()));
    if (!usable) return;
    const auto key=bucket_of(w.at);
    if (!open_bucket(key)) return;
    auto& b=buckets[key];++b.radar;
    if (j.contains("presence") && j["presence"].is_boolean() && j["presence"].get<bool>()) ++b.presence;
  }
  void flush(int64_t before) {
    disk_full=false;
    records->make_room();
    exec(db,"BEGIN IMMEDIATE");
    try {
      Statement prune(db,"DELETE FROM environment_buckets WHERE bucket_s < ?");
      prune.integer(1,static_cast<int64_t>(cfg.now())-int64_t(cfg.retention_days)*86400); prune.done(db);
      for (const auto& [key,b]:buckets) {
        if (key>=before) continue;
        Statement s(db,R"SQL(INSERT INTO environment_buckets VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(bucket_s) DO UPDATE SET
 total_count=total_count+excluded.total_count,valid_count=valid_count+excluded.valid_count,
 temp_sum=coalesce(temp_sum,0)+coalesce(excluded.temp_sum,0),
 temp_min=min(coalesce(temp_min,excluded.temp_min),coalesce(excluded.temp_min,temp_min)),
 temp_max=max(coalesce(temp_max,excluded.temp_max),coalesce(excluded.temp_max,temp_max)),
 humidity_sum=coalesce(humidity_sum,0)+coalesce(excluded.humidity_sum,0),
 humidity_min=min(coalesce(humidity_min,excluded.humidity_min),coalesce(excluded.humidity_min,humidity_min)),
 humidity_max=max(coalesce(humidity_max,excluded.humidity_max),coalesce(excluded.humidity_max,humidity_max)),
 co2_sum=coalesce(co2_sum,0)+coalesce(excluded.co2_sum,0),
 co2_min=min(coalesce(co2_min,excluded.co2_min),coalesce(excluded.co2_min,co2_min)),
 co2_max=max(coalesce(co2_max,excluded.co2_max),coalesce(excluded.co2_max,co2_max)),
 radar_count=radar_count+excluded.radar_count,presence_count=presence_count+excluded.presence_count)SQL");
        s.integer(1,key);s.integer(2,b.total);s.integer(3,b.valid);
        if (b.valid) {
          int i=4;
          for (const auto& m:{b.t,b.h,b.c}) {s.real(i++,m.sum);s.real(i++,m.lo);s.real(i++,m.hi);}
        }
        s.integer(13,b.radar);s.integer(14,b.presence);
        s.done(db);
      }
      // Only checkpoint identities if ALL pending samples are persisted. Otherwise a crash
      // could mark a current-bucket sample seen without its corresponding aggregate.
      if (before==kMaxTime) {
        const double cutoff=cfg.now()-int64_t(cfg.retention_days)*86400;
        for(auto it=sources.begin();it!=sources.end();) {
          if(source_seen[it->first]<cutoff){source_seen.erase(it->first);it=sources.erase(it);continue;}
          Statement s(db,"INSERT INTO environment_sources(source,sequence,seen_s) VALUES(?,?,?) ON CONFLICT(source) DO UPDATE SET sequence=excluded.sequence,seen_s=excluded.seen_s");
          s.text(1,it->first);s.integer(2,it->second);s.real(3,source_seen[it->first]);s.done(db);++it;
        }
        Statement prune_sources(db,"DELETE FROM environment_sources WHERE seen_s<?");prune_sources.real(1,cutoff);prune_sources.done(db);
      }
      records->write(cfg.now()-int64_t(cfg.retention_days)*86400);
      sqlite3_reset(prune.p); prune.done(db); // also remove expired pending buckets after a forward clock jump
      exec(db,"COMMIT");
      records->committed();
      for (auto it=buckets.begin();it!=buckets.end();) { if (it->first<before) it=buckets.erase(it); else ++it; }
      // Checkpoints are left to wal_autocheckpoint; forcing one per commit doubled SD card writes.
      last_error.clear();
    } catch (...) {
      const int code=sqlite3_errcode(db);
      sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
      disk_full=code==SQLITE_FULL;
      if(disk_full)records->make_room(true);
      throw;
    }
  }
  Json select(const Work& w) {
    Statement s(db,"SELECT * FROM environment_buckets WHERE bucket_s>=? AND bucket_s<? ORDER BY bucket_s LIMIT ?");
    s.integer(1,w.from);s.integer(2,w.to);s.integer(3,int64_t(w.limit)+1);
    Json rows=Json::array();
    int result;
    while ((result=sqlite3_step(s.p))==SQLITE_ROW) {
      Json row{{"bucket_s",sqlite3_column_int64(s.p,0)},{"total_count",sqlite3_column_int(s.p,1)},{"valid_count",sqlite3_column_int(s.p,2)}};
      const int n=sqlite3_column_int(s.p,2);
      int i=3;
      for (const char* name:{"temperature_c","humidity_pct","co2_ppm"}) {
        row[name]=n ? Json{{"mean",sqlite3_column_double(s.p,i)/n},{"min",sqlite3_column_double(s.p,i+1)},{"max",sqlite3_column_double(s.p,i+2)}} : Json(nullptr);
        i+=3;
      }
      const int radar=sqlite3_column_int(s.p,12);
      row["radar_count"]=radar;
      row["presence_ratio"]=radar ? Json(double(sqlite3_column_int(s.p,13))/radar) : Json(nullptr);
      rows.push_back(std::move(row));
    }
    if (result!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    const bool truncated=rows.size()>w.limit;
    if (truncated) rows.erase(rows.end()-1);
    uint64_t lost;
    {std::lock_guard<std::mutex> lock(mutex);lost=dropped;}
    return Json{{"ok",true},{"time_unit","unix_seconds"},{"bucket_seconds",cfg.bucket_s},{"records",rows},{"truncated",truncated},
      {"dropped_samples",lost},{"limited_by_size",records->limited("environment_buckets",w.from)}};
  }
  void drain() {
    // Each chunk is atomic. Keep uncommitted event payloads queued for the next chunk.
    // Bounded retries allow high-water eviction to free space for a large event burst.
    int retries=0;
    while(true) {
      try { flush(kMaxTime); }
      catch(...) { if(!disk_full || ++retries>16)throw; else continue; }
      retries=0;
      if(!records->has_events())break;
    }
  }
  void run() {
    auto next_flush=std::chrono::steady_clock::now()+std::chrono::seconds(cfg.flush_s);
    while (true) {
      std::deque<Work> batch;
      bool stop;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock,std::chrono::seconds(1),[&]{return stopping || !queue.empty();});
        batch.swap(queue);stop=stopping;
      }
      for (auto& w:batch) {
        try {
          if (w.reply) {
            if(w.kind=="import") {
              drain();
              const auto n=records->import_file(w.sample.get<std::string>(),cfg.now(),cfg.retention_days);
              drain();w.reply->set_value(Json{{"ok",true},{"imported",n}});
            } else {
              drain();
              Json result=w.kind=="summary"?records->summary(w.from,w.to,w.limit):
                w.kind=="events"?records->events(w.from,w.to,w.limit,w.after):select(w);
              {std::lock_guard<std::mutex> lock(mutex);result["dropped_samples"]=dropped;}
              w.reply->set_value(std::move(result));
            }
          } else if(w.topic=="sensor/env") ingest(w);
          else if(w.topic=="sensor/radar") radar(w);
          else records->ingest(w.topic,w.sample,w.at);
        } catch (const std::exception& e) {
          last_error=e.what();
          if (w.reply) w.reply->set_value(failure(last_error));
          std::cerr<<"[history] "<<last_error<<"\n";
        }
      }
      try {
        if (stop) {drain();break;}
        // Batch everything pending into one commit every flush_s (SD card wear).
        if ((!buckets.empty() || records->dirty()) && std::chrono::steady_clock::now()>=next_flush) {
          flush(kMaxTime);next_flush=std::chrono::steady_clock::now()+std::chrono::seconds(cfg.flush_s);
        }
      } catch (const std::exception& e) {
        std::cerr<<"[history] "<<e.what()<<"\n";
        if (stop) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
  }
};
HistoryStore::HistoryStore(Config c):impl_(std::make_unique<Impl>(std::move(c))) {}
HistoryStore::~HistoryStore()=default;
bool HistoryStore::observe(const Json& sample,double at,bool retained) {
  return record("sensor/env",sample,at,retained);
}
bool HistoryStore::record(const std::string& topic,const Json& sample,double at,bool retained) {
  // Microphone results are not stored (cry episodes come from cry/*).
  if(topic!="sensor/env"&&topic!="sensor/radar"&&topic.rfind("cry/",0)!=0&&
     topic!="fusion/alert"&&topic!="event/tof"&&topic.rfind("control/",0)!=0)return false;
  if (retained || !sample.is_object() || !std::isfinite(at) || at<0 || at>kMaxTime) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->queue.size()>=4096) {++impl_->dropped;return false;}
  if(sample.dump().size()>32768) {++impl_->dropped;return false;}
  Impl::Work work;work.sample=sample;work.at=at;work.topic=topic;
  impl_->queue.push_back(std::move(work));impl_->wake.notify_one();return true;
}
Json HistoryStore::query(int64_t from,int64_t to,uint32_t limit) { return request("environment",from,to,limit); }
Json HistoryStore::summary(int64_t from,int64_t to,uint32_t limit) { return request("summary",from,to,limit); }
Json HistoryStore::events(int64_t from,int64_t to,uint32_t limit,int64_t after) { return request("events",from,to,limit,after); }
Json HistoryStore::import_cry_log(const std::string& path) {
  auto reply=std::make_shared<std::promise<Json>>();auto result=reply->get_future();
  {std::lock_guard<std::mutex> lock(impl_->mutex);
   if(impl_->queue.size()>=4096)return failure("history queue busy; retry");
   Impl::Work w;w.kind="import";w.sample=path;w.reply=reply;impl_->queue.push_back(std::move(w));}
  impl_->wake.notify_one();return result.get();
}
Json HistoryStore::request(const std::string& kind,int64_t from,int64_t to,uint32_t limit,int64_t after) {
  if (from<0 || to>kMaxTime || from>=to || to-from>31LL*86400 || limit<1 || limit>10080 || (kind=="events"&&limit>1000) || after<0)
    return failure(std::string("invalid range: use Unix seconds, from < to, at most 31 days and limit 1..")+(kind=="events"?"1000":"10080"));
  auto reply=std::make_shared<std::promise<Json>>();auto result=reply->get_future();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->queue.size()>=4096) return failure("history queue busy; retry");
    Impl::Work work;work.kind=kind;work.after=after;work.from=from;work.to=to;work.limit=limit;work.reply=reply;
    impl_->queue.push_back(std::move(work));
  }
  impl_->wake.notify_one();return result.get();
}
}
