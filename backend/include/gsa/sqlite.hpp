// Small RAII layer over SQLite for read-only data packs, with a connection pool.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace gsa::sql {

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

using Value = std::variant<std::monostate, int64_t, double, std::string>;

class Row {
 public:
  explicit Row(sqlite3_stmt* s) : s_(s) {}
  bool is_null(int i) const;
  int64_t i64(int i) const;
  double f64(int i) const;
  std::string text(int i) const;
  std::optional<double> opt_f64(int i) const;
  std::optional<std::string> opt_text(int i) const;

 private:
  sqlite3_stmt* s_;
};

class Db {
 public:
  explicit Db(const std::filesystem::path& path, bool readonly = true);
  ~Db();
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  // Runs sql with positional binds; calls fn for each row. Returns the row count.
  size_t query(const std::string& sql, const std::vector<Value>& binds, const std::function<void(const Row&)>& fn);
  void exec(const std::string& sql);
  sqlite3* raw() { return db_; }

 private:
  sqlite3* db_ = nullptr;
};

// A fixed set of connections to one file. lease() blocks until one is free.
class Pool {
 public:
  Pool(std::filesystem::path path, size_t size);
  const std::filesystem::path& path() const { return path_; }

  class Lease {
   public:
    Lease(Pool& p, std::unique_ptr<Db> db) : p_(&p), db_(std::move(db)) {}
    Lease(Lease&& o) noexcept : p_(o.p_), db_(std::move(o.db_)) { o.p_ = nullptr; }
    ~Lease();
    Db* operator->() { return db_.get(); }
    Db& operator*() { return *db_; }

   private:
    Pool* p_;
    std::unique_ptr<Db> db_;
  };
  Lease lease();

 private:
  friend class Lease;
  std::filesystem::path path_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::unique_ptr<Db>> free_;
};

}  // namespace gsa::sql
