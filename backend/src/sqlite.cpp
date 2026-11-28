#include "gsa/sqlite.hpp"

#include <sqlite3.h>

namespace gsa::sql {

bool Row::is_null(int i) const { return sqlite3_column_type(s_, i) == SQLITE_NULL; }
int64_t Row::i64(int i) const { return sqlite3_column_int64(s_, i); }
double Row::f64(int i) const { return sqlite3_column_double(s_, i); }
std::string Row::text(int i) const {
  const auto* t = sqlite3_column_text(s_, i);
  return t ? std::string(reinterpret_cast<const char*>(t), size_t(sqlite3_column_bytes(s_, i))) : std::string();
}
std::string Row::blob(int i) const {
  const auto* b = static_cast<const char*>(sqlite3_column_blob(s_, i));
  return b ? std::string(b, size_t(sqlite3_column_bytes(s_, i))) : std::string();
}
std::optional<double> Row::opt_f64(int i) const {
  if (is_null(i)) return std::nullopt;
  return f64(i);
}
std::optional<std::string> Row::opt_text(int i) const {
  if (is_null(i)) return std::nullopt;
  return text(i);
}

Db::Db(const std::filesystem::path& path, bool readonly) {
  const int flags = (readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) | SQLITE_OPEN_NOMUTEX;
  const std::string p = path.u8string().empty() ? std::string() : reinterpret_cast<const char*>(path.u8string().c_str());
  if (sqlite3_open_v2(p.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
    std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
    sqlite3_close(db_);
    db_ = nullptr;
    throw Error("cannot open " + p + ": " + msg);
  }
  sqlite3_busy_timeout(db_, 2000);
  if (readonly) exec("PRAGMA query_only=1");
}

Db::~Db() { sqlite3_close_v2(db_); }

void Db::exec(const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "error";
    sqlite3_free(err);
    throw Error(msg);
  }
}

size_t Db::query(const std::string& sql, const std::vector<Value>& binds, const std::function<void(const Row&)>& fn) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), int(sql.size()), &st, nullptr) != SQLITE_OK) {
    throw Error(std::string("prepare: ") + sqlite3_errmsg(db_));
  }
  std::unique_ptr<sqlite3_stmt, int (*)(sqlite3_stmt*)> guard(st, sqlite3_finalize);
  for (size_t i = 0; i < binds.size(); ++i) {
    const int at = int(i + 1);
    std::visit(
        [&](auto&& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, std::monostate>) sqlite3_bind_null(st, at);
          else if constexpr (std::is_same_v<T, int64_t>) sqlite3_bind_int64(st, at, v);
          else if constexpr (std::is_same_v<T, double>) sqlite3_bind_double(st, at, v);
          else sqlite3_bind_text(st, at, v.data(), int(v.size()), SQLITE_TRANSIENT);
        },
        binds[i]);
  }
  size_t n = 0;
  for (;;) {
    const int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) throw Error(std::string("step: ") + sqlite3_errmsg(db_));
    fn(Row(st));
    ++n;
  }
  return n;
}

Pool::Pool(std::filesystem::path path, size_t size) : path_(std::move(path)) {
  for (size_t i = 0; i < std::max<size_t>(1, size); ++i) free_.push_back(std::make_unique<Db>(path_));
}

Pool::Lease Pool::lease() {
  std::unique_lock<std::mutex> lk(mu_);
  cv_.wait(lk, [&] { return !free_.empty(); });
  auto db = std::move(free_.back());
  free_.pop_back();
  return Lease(*this, std::move(db));
}

Pool::Lease::~Lease() {
  if (!p_ || !db_) return;
  {
    std::lock_guard<std::mutex> lk(p_->mu_);
    p_->free_.push_back(std::move(db_));
  }
  p_->cv_.notify_one();
}

}  // namespace gsa::sql
