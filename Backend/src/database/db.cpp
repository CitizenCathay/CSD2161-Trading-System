#include "database/db.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <sqlite3.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace database
{
  static std::string get_env_string(const char* name)
  {
    char buffer[4096]{};
    const DWORD length = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (length == 0 || length >= sizeof(buffer))
    {
      return {};
    }
    return std::string(buffer, buffer + length);
  }

  std::string default_db_path()
  {
    const auto env = get_env_string("DATABASE");
    if (!env.empty())
    {
      return env;
    }
    return "backend.sqlite";
  }

  struct SqliteStmt
  {
    sqlite3_stmt* handle{};
    ~SqliteStmt()
    {
      if (handle)
      {
        sqlite3_finalize(handle);
      }
    }
  };

  Db::Db(sqlite3* db) : db_(db) {}
  Db::~Db() { close(); }
  Db::Db(Db&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

  Db& Db::operator=(Db&& other) noexcept
  {
    if (this == &other) return *this;
    close();
    db_ = std::exchange(other.db_, nullptr);
    return *this;
  }

  void Db::close()
  {
    if (db_)
    {
      sqlite3_close(db_);
      db_ = nullptr;
    }
  }

  sqlite3* Db::handle() const { return db_; }

  std::optional<Db> Db::open(const std::string& path)
  {
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK || !db)
    {
      if (db) sqlite3_close(db);
      return std::nullopt;
    }

    sqlite3_busy_timeout(db, 3000);

    const char* init_sql =
      "PRAGMA journal_mode=WAL;"

      "CREATE TABLE IF NOT EXISTS users ("
      "  username   TEXT PRIMARY KEY,"
      "  salt_b64   TEXT NOT NULL,"
      "  hash_b64   TEXT NOT NULL,"
      "  created_at INTEGER NOT NULL"
      ");"

      "CREATE TABLE IF NOT EXISTS accounts ("
      "  username TEXT PRIMARY KEY,"
      "  cash     REAL NOT NULL DEFAULT 100000.0,"
      "  reserved_cash REAL NOT NULL DEFAULT 0.0"
      ");"

      "CREATE TABLE IF NOT EXISTS holdings ("
      "  username TEXT    NOT NULL,"
      "  symbol   TEXT    NOT NULL,"
      "  qty      REAL    NOT NULL DEFAULT 0.0,"
      "  PRIMARY KEY (username, symbol)"
      ");"

      "CREATE TABLE IF NOT EXISTS orders ("
      "  order_id      INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  user_id       TEXT    NOT NULL,"
      "  symbol        TEXT    NOT NULL,"
      "  side          TEXT    NOT NULL,"
      "  price         REAL    NOT NULL,"
      "  qty_total     REAL    NOT NULL,"
      "  qty_remaining REAL    NOT NULL,"
      "  arrival       INTEGER NOT NULL,"
      "  status        TEXT    NOT NULL"
      ");"

      "CREATE TABLE IF NOT EXISTS trades ("
      "  trade_id      INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  buy_order_id  INTEGER NOT NULL,"
      "  sell_order_id INTEGER NOT NULL,"
      "  buyer_id      TEXT    NOT NULL,"
      "  seller_id     TEXT    NOT NULL,"
      "  symbol        TEXT    NOT NULL,"
      "  qty           REAL    NOT NULL,"
      "  price         REAL    NOT NULL,"
      "  timestamp     INTEGER NOT NULL"
      ");";

    char* err = nullptr;
    const int init_rc = sqlite3_exec(db, init_sql, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    if (init_rc != SQLITE_OK)
    {
      sqlite3_close(db);
      return std::nullopt;
    }

    sqlite3_exec(db, "ALTER TABLE accounts ADD COLUMN reserved_cash REAL NOT NULL DEFAULT 0.0;", nullptr, nullptr, nullptr);

    return Db(db);
  }

  // ---- transactions ----
  
  // Wrapping multiple DB calls in BEGIN/COMMIT makes them atomic:
  // either all succeed together or none are saved

  bool Db::begin_transaction() const
  {
      // Start recording changes but don't save anything yet
      return sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr) == SQLITE_OK;
  }

  bool Db::commit() const
  {
	  // All changes since BEGIN look good; save them permanently
      return sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK;
  }

  bool Db::rollback() const
  {
	  // Something went wrong; discard all changes since BEGIN and revert to last COMMIT
      return sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr) == SQLITE_OK;
  }

  // ---- users ----

  std::optional<models::UserRecord> Db::get_user(const std::string& username) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT salt_b64, hash_b64 FROM users WHERE username=?1 LIMIT 1;", -1, &raw, nullptr) != SQLITE_OK)
      return std::nullopt;
    SqliteStmt stmt{ raw };
    sqlite3_bind_text(stmt.handle, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.handle) != SQLITE_ROW) return std::nullopt;
    const unsigned char* salt = sqlite3_column_text(stmt.handle, 0);
    const unsigned char* hash = sqlite3_column_text(stmt.handle, 1);
    if (!salt || !hash) return std::nullopt;
    models::UserRecord record;
    record.salt_b64 = reinterpret_cast<const char*>(salt);
    record.hash_b64 = reinterpret_cast<const char*>(hash);
    return record;
  }

  std::optional<bool> Db::user_exists(const std::string& username) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT 1 FROM users WHERE username=?1 LIMIT 1;", -1, &raw, nullptr) != SQLITE_OK)
      return std::nullopt;
    SqliteStmt stmt{ raw };
    sqlite3_bind_text(stmt.handle, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    const int step = sqlite3_step(stmt.handle);
    if (step == SQLITE_ROW)  return true;
    if (step == SQLITE_DONE) return false;
    return std::nullopt;
  }

  InsertUserResult Db::insert_user(const std::string& username, const models::UserRecord& record, std::int64_t created_at) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
      "INSERT INTO users(username,salt_b64,hash_b64,created_at) VALUES(?1,?2,?3,?4);",
      -1, &raw, nullptr) != SQLITE_OK)
      return InsertUserResult::error;
    SqliteStmt stmt{ raw };
    sqlite3_bind_text(stmt.handle, 1, username.c_str(),        -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.handle, 2, record.salt_b64.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.handle, 3, record.hash_b64.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.handle, 4, created_at);
    const int step = sqlite3_step(stmt.handle);
    if (step == SQLITE_DONE)       return InsertUserResult::ok;
    if (step == SQLITE_CONSTRAINT) return InsertUserResult::exists;
    if (sqlite3_errcode(db_) == SQLITE_CONSTRAINT) return InsertUserResult::exists;
    return InsertUserResult::error;
  }

  std::optional<std::vector<UserSummary>> Db::list_users() const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT username, created_at FROM users ORDER BY created_at DESC;", -1, &raw, nullptr) != SQLITE_OK)
      return std::nullopt;
    SqliteStmt stmt{ raw };
    std::vector<UserSummary> users;
    while (true)
    {
      const int step = sqlite3_step(stmt.handle);
      if (step == SQLITE_DONE) break;
      if (step != SQLITE_ROW)  return std::nullopt;
      const unsigned char* uname = sqlite3_column_text(stmt.handle, 0);
      if (!uname) continue;
      UserSummary u;
      u.username   = reinterpret_cast<const char*>(uname);
      u.created_at = sqlite3_column_int64(stmt.handle, 1);
      users.push_back(std::move(u));
    }
    return users;
  }

  // ---- accounts ----

  std::optional<double> Db::get_cash(const std::string& username) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT cash FROM accounts WHERE username=?1 LIMIT 1;", -1, &raw, nullptr) != SQLITE_OK)
      return std::nullopt;
    SqliteStmt stmt{ raw };
    sqlite3_bind_text(stmt.handle, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.handle) != SQLITE_ROW) return std::nullopt;
    return sqlite3_column_double(stmt.handle, 0);
  }

  bool Db::upsert_account(const std::string& username, double cash) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
      "INSERT INTO accounts(username,cash) VALUES(?1,?2) "
      "ON CONFLICT(username) DO UPDATE SET cash=excluded.cash;",
      -1, &raw, nullptr) != SQLITE_OK)
      return false;
    SqliteStmt stmt{ raw };
    sqlite3_bind_text(stmt.handle,   1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt.handle, 2, cash);
    return sqlite3_step(stmt.handle) == SQLITE_DONE;
  }

  std::optional<double> Db::get_reserved_cash(const std::string& username) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT reserved_cash FROM accounts WHERE username=?1 LIMIT 1;", -1, &raw, nullptr) != SQLITE_OK)
      return std::nullopt;
    SqliteStmt stmt{ raw };
    sqlite3_bind_text(stmt.handle, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.handle) != SQLITE_ROW) return std::nullopt;
    return sqlite3_column_double(stmt.handle, 0);
  }

  bool Db::upsert_account_with_reserved(const std::string& username, double cash, double reserved_cash) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
      "INSERT INTO accounts(username,cash,reserved_cash) VALUES(?1,?2,?3) "
      "ON CONFLICT(username) DO UPDATE SET cash=excluded.cash, reserved_cash=excluded.reserved_cash;",
      -1, &raw, nullptr) != SQLITE_OK)
      return false;
    SqliteStmt stmt{ raw };
    sqlite3_bind_text(stmt.handle,   1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt.handle, 2, cash);
    sqlite3_bind_double(stmt.handle, 3, reserved_cash);
    return sqlite3_step(stmt.handle) == SQLITE_DONE;
  }

  std::optional<double> Db::get_holdings(const std::string& username, const std::string& symbol) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
      "SELECT qty FROM holdings WHERE username=?1 AND symbol=?2 LIMIT 1;",
      -1, &raw, nullptr) != SQLITE_OK)
      return std::nullopt;
    SqliteStmt stmt{ raw };
    sqlite3_bind_text(stmt.handle, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.handle, 2, symbol.c_str(),   -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.handle) != SQLITE_ROW) return 0.0; // no row = 0 holdings
    return sqlite3_column_double(stmt.handle, 0);
  }

  bool Db::upsert_holdings(const std::string& username, const std::string& symbol, double qty) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
      "INSERT INTO holdings(username,symbol,qty) VALUES(?1,?2,?3) "
      "ON CONFLICT(username,symbol) DO UPDATE SET qty=excluded.qty;",
      -1, &raw, nullptr) != SQLITE_OK)
      return false;
    SqliteStmt stmt{ raw };
    sqlite3_bind_text(stmt.handle, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.handle, 2, symbol.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt.handle, 3, qty);
    return sqlite3_step(stmt.handle) == SQLITE_DONE;
  }

  std::vector<std::pair<std::string, double>> Db::list_holdings(const std::string& username) const
  {
    std::vector<std::pair<std::string, double>> result;
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
      "SELECT symbol, qty FROM holdings WHERE username=?1 AND qty>0 ORDER BY symbol ASC;",
      -1, &raw, nullptr) != SQLITE_OK)
      return result;
    SqliteStmt stmt{ raw };
    sqlite3_bind_text(stmt.handle, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    while (true)
    {
      const int step = sqlite3_step(stmt.handle);
      if (step == SQLITE_DONE) break;
      if (step != SQLITE_ROW) break;
      const unsigned char* sym = sqlite3_column_text(stmt.handle, 0);
      const double qty = sqlite3_column_double(stmt.handle, 1);
      if (!sym) continue;
      result.emplace_back(reinterpret_cast<const char*>(sym), qty);
    }
    return result;
  }

  // ---- orders ----

  static models::Side side_from_string(const char* s)
  {
    return (s && std::string(s) == "buy") ? models::Side::Buy : models::Side::Sell;
  }

  static models::OrderStatus status_from_string(const char* s)
  {
    if (!s) return models::OrderStatus::Open;
    std::string sv(s);
    if (sv == "partial_fill") return models::OrderStatus::PartialFill;
    if (sv == "filled")       return models::OrderStatus::Filled;
    if (sv == "cancelled")    return models::OrderStatus::Cancelled;
    return models::OrderStatus::Open;
  }

  static const char* side_to_string(models::Side side)
  {
    return side == models::Side::Buy ? "buy" : "sell";
  }

  static const char* status_to_string(models::OrderStatus status)
  {
    switch (status)
    {
    case models::OrderStatus::PartialFill: return "partial_fill";
    case models::OrderStatus::Filled:      return "filled";
    case models::OrderStatus::Cancelled:   return "cancelled";
    default:                               return "open";
    }
  }

  std::optional<std::int64_t> Db::insert_order(const models::Order& order) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
      "INSERT INTO orders(user_id,symbol,side,price,qty_total,qty_remaining,arrival,status) "
      "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
      -1, &raw, nullptr) != SQLITE_OK)
      return std::nullopt;
    SqliteStmt stmt{ raw };
    sqlite3_bind_text(stmt.handle,   1, order.user_id.c_str(),        -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.handle,   2, order.symbol.c_str(),         -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.handle,   3, side_to_string(order.side),   -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt.handle, 4, order.price);
    sqlite3_bind_double(stmt.handle, 5, order.qty_total);
    sqlite3_bind_double(stmt.handle, 6, order.qty_remaining);
    sqlite3_bind_int64(stmt.handle,  7, order.arrival);
    sqlite3_bind_text(stmt.handle,   8, status_to_string(order.status), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt.handle) != SQLITE_DONE) return std::nullopt;
    return static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_));
  }

  bool Db::update_order(const models::Order& order) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
      "UPDATE orders SET qty_remaining=?1, status=?2 WHERE order_id=?3;",
      -1, &raw, nullptr) != SQLITE_OK)
      return false;
    SqliteStmt stmt{ raw };
    sqlite3_bind_double(stmt.handle, 1, order.qty_remaining);
    sqlite3_bind_text(stmt.handle,  2, status_to_string(order.status), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt.handle, 3, order.order_id);
    return sqlite3_step(stmt.handle) == SQLITE_DONE;
  }

  CancelOrderCheck Db::check_cancel_order(std::int64_t order_id, const std::string& user_id) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
        "SELECT user_id, status FROM orders WHERE order_id=?1 LIMIT 1;",
        -1, &raw, nullptr) != SQLITE_OK)
      return CancelOrderCheck::error;
    SqliteStmt stmt{ raw };
    sqlite3_bind_int64(stmt.handle, 1, order_id);
    const int step = sqlite3_step(stmt.handle);
    if (step == SQLITE_DONE) return CancelOrderCheck::not_found;
    if (step != SQLITE_ROW)  return CancelOrderCheck::error;

    const unsigned char* uid = sqlite3_column_text(stmt.handle, 0);
    const unsigned char* status = sqlite3_column_text(stmt.handle, 1);
    const std::string owner = uid ? reinterpret_cast<const char*>(uid) : "";
    if (owner != user_id) return CancelOrderCheck::not_owner;

    const std::string status_value = status ? reinterpret_cast<const char*>(status) : "";
    if (status_value != "open" && status_value != "partial_fill")
      return CancelOrderCheck::already_closed;

    return CancelOrderCheck::ok;
  }

  std::optional<models::Order> Db::get_order_by_id(std::int64_t order_id) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
      "SELECT order_id,user_id,symbol,side,price,qty_total,qty_remaining,arrival,status "
      "FROM orders WHERE order_id=?1 LIMIT 1;",
      -1, &raw, nullptr) != SQLITE_OK)
      return std::nullopt;
    SqliteStmt stmt{ raw };
    sqlite3_bind_int64(stmt.handle, 1, order_id);
    if (sqlite3_step(stmt.handle) != SQLITE_ROW) return std::nullopt;

    models::Order o;
    o.order_id       = sqlite3_column_int64(stmt.handle, 0);
    const unsigned char* uid = sqlite3_column_text(stmt.handle, 1);
    const unsigned char* sym = sqlite3_column_text(stmt.handle, 2);
    const unsigned char* sid = sqlite3_column_text(stmt.handle, 3);
    const unsigned char* sts = sqlite3_column_text(stmt.handle, 8);
    o.user_id        = uid ? reinterpret_cast<const char*>(uid) : "";
    o.symbol         = sym ? reinterpret_cast<const char*>(sym) : "";
    o.side           = side_from_string(reinterpret_cast<const char*>(sid));
    o.price          = sqlite3_column_double(stmt.handle, 4);
    o.qty_total      = sqlite3_column_double(stmt.handle, 5);
    o.qty_remaining  = sqlite3_column_double(stmt.handle, 6);
    o.arrival        = sqlite3_column_int64(stmt.handle, 7);
    o.status         = status_from_string(reinterpret_cast<const char*>(sts));
    return o;
  }

  // When a user wants to cancel an order, they send: DELETE /orders
  // { "order_id": 123 }
  bool Db::cancel_order(std::int64_t order_id, const std::string& user_id) const
  {
	  // order_id = which order to cancel
	  // user_id = who is requesting the cancellation; we check this matches the order's user_id for security
      // OTHERWISE ANY LOGGED-IN USER CAN CANCEL ANYONE ELSE'S ORDERS
      sqlite3_stmt* raw = nullptr;
      if (sqlite3_prepare_v2(db_,
          "UPDATE orders SET status='cancelled' "
          "WHERE order_id=?1 AND user_id=?2 AND (status='open' OR status='partial_fill');",
          -1, &raw, nullptr) != SQLITE_OK)
          return false;

      SqliteStmt stmt{ raw };

      // Plug actual values into placeholders ?1 and ?2
	  sqlite3_bind_int64(stmt.handle, 1, order_id);                             // ?1 = order_id
	  sqlite3_bind_text(stmt.handle, 2, user_id.c_str(), -1, SQLITE_TRANSIENT); // ?2 = user_id

	  // Update only succeeds if both order_id and user_id match an open/partial_fill order; 
      // otherwise no rows are updated
      if (sqlite3_step(stmt.handle) != SQLITE_DONE) return false;
      return sqlite3_changes(db_) > 0;
  }

  // Returns the highest arrival sequence number in the orders table
  // Used on startup to restore seq_ so new orders don't reuse old sequence numbers
  std::int64_t Db::get_max_arrival() const
  {
      /*
      Example:
        * A places sell order -> arrival = 1, saved to DB
        * Server restarts
        * B places buy order -> arrival = 1 again (seq_ resets to 0, then ++seq_ = 1)
        * Now both have arrival = 1, regarded as having arrived at the same time -> uses midpoint price 
          instead of resting-order price
      */
      sqlite3_stmt* raw = nullptr;

      // MAX(arrival) = highest sequence number; COALESCE returns 0 if table is empty (MAX returns NULL)
      if (sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(arrival), 0) FROM orders;", -1, &raw, nullptr) != SQLITE_OK)
          return 0;                        

      SqliteStmt stmt{ raw };
      if (sqlite3_step(stmt.handle) != SQLITE_ROW) return 0;  
      return sqlite3_column_int64(stmt.handle, 0);  
  }

  std::vector<models::Order> Db::get_open_orders(const std::string& symbol) const
  {
    sqlite3_stmt* raw = nullptr;
    // buys: highest price first, earliest arrival first
    // sells: lowest price first, earliest arrival first
    // We return all open/partial, caller sorts by side
    if (sqlite3_prepare_v2(db_,
      "SELECT order_id,user_id,symbol,side,price,qty_total,qty_remaining,arrival,status "
      "FROM orders WHERE symbol=?1 AND (status='open' OR status='partial_fill') "
      "ORDER BY side ASC, price DESC, arrival ASC;",
      -1, &raw, nullptr) != SQLITE_OK)
      return {};
    SqliteStmt stmt{ raw };
    sqlite3_bind_text(stmt.handle, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<models::Order> orders;
    while (sqlite3_step(stmt.handle) == SQLITE_ROW)
    {
      models::Order o;
      o.order_id       = sqlite3_column_int64(stmt.handle, 0);
      const unsigned char* uid = sqlite3_column_text(stmt.handle, 1);
      const unsigned char* sym = sqlite3_column_text(stmt.handle, 2);
      const unsigned char* sid = sqlite3_column_text(stmt.handle, 3);
      const unsigned char* sts = sqlite3_column_text(stmt.handle, 8);
      o.user_id        = uid ? reinterpret_cast<const char*>(uid) : "";
      o.symbol         = sym ? reinterpret_cast<const char*>(sym) : "";
      o.side           = side_from_string(reinterpret_cast<const char*>(sid));
      o.price          = sqlite3_column_double(stmt.handle, 4);
      o.qty_total      = sqlite3_column_double(stmt.handle, 5);
      o.qty_remaining  = sqlite3_column_double(stmt.handle, 6);
      o.arrival        = sqlite3_column_int64(stmt.handle, 7);
      o.status         = status_from_string(reinterpret_cast<const char*>(sts));
      orders.push_back(std::move(o));
    }
    return orders;
  }

  std::vector<models::Order> Db::get_orders_by_user(const std::string& username) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
      "SELECT order_id,user_id,symbol,side,price,qty_total,qty_remaining,arrival,status "
      "FROM orders WHERE user_id=?1 ORDER BY arrival DESC;",
      -1, &raw, nullptr) != SQLITE_OK)
      return {};
    SqliteStmt stmt{ raw };
    sqlite3_bind_text(stmt.handle, 1, username.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<models::Order> orders;
    while (sqlite3_step(stmt.handle) == SQLITE_ROW)
    {
      models::Order o;
      o.order_id       = sqlite3_column_int64(stmt.handle, 0);
      const unsigned char* uid = sqlite3_column_text(stmt.handle, 1);
      const unsigned char* sym = sqlite3_column_text(stmt.handle, 2);
      const unsigned char* sid = sqlite3_column_text(stmt.handle, 3);
      const unsigned char* sts = sqlite3_column_text(stmt.handle, 8);
      o.user_id        = uid ? reinterpret_cast<const char*>(uid) : "";
      o.symbol         = sym ? reinterpret_cast<const char*>(sym) : "";
      o.side           = side_from_string(reinterpret_cast<const char*>(sid));
      o.price          = sqlite3_column_double(stmt.handle, 4);
      o.qty_total      = sqlite3_column_double(stmt.handle, 5);
      o.qty_remaining  = sqlite3_column_double(stmt.handle, 6);
      o.arrival        = sqlite3_column_int64(stmt.handle, 7);
      o.status         = status_from_string(reinterpret_cast<const char*>(sts));
      orders.push_back(std::move(o));
    }
    return orders;
  }

  // ---- trades ----

  bool Db::insert_trade(const models::Trade& trade) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
      "INSERT INTO trades(buy_order_id,sell_order_id,buyer_id,seller_id,symbol,qty,price,timestamp) "
      "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
      -1, &raw, nullptr) != SQLITE_OK)
      return false;
    SqliteStmt stmt{ raw };
    sqlite3_bind_int64(stmt.handle,  1, trade.buy_order_id);
    sqlite3_bind_int64(stmt.handle,  2, trade.sell_order_id);
    sqlite3_bind_text(stmt.handle,   3, trade.buyer_id.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.handle,   4, trade.seller_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.handle,   5, trade.symbol.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt.handle, 6, trade.qty);
    sqlite3_bind_double(stmt.handle, 7, trade.price);
    sqlite3_bind_int64(stmt.handle,  8, trade.timestamp);
    return sqlite3_step(stmt.handle) == SQLITE_DONE;
  }

  std::vector<models::Trade> Db::get_trade_history(const std::string& symbol) const
  {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
      "SELECT trade_id,buy_order_id,sell_order_id,buyer_id,seller_id,symbol,qty,price,timestamp "
      "FROM trades WHERE symbol=?1 ORDER BY timestamp DESC LIMIT 100;",
      -1, &raw, nullptr) != SQLITE_OK)
      return {};
    SqliteStmt stmt{ raw };
    sqlite3_bind_text(stmt.handle, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<models::Trade> trades;
    while (sqlite3_step(stmt.handle) == SQLITE_ROW)
    {
      models::Trade t;
      t.trade_id      = sqlite3_column_int64(stmt.handle, 0);
      t.buy_order_id  = sqlite3_column_int64(stmt.handle, 1);
      t.sell_order_id = sqlite3_column_int64(stmt.handle, 2);
      const unsigned char* bid = sqlite3_column_text(stmt.handle, 3);
      const unsigned char* sid = sqlite3_column_text(stmt.handle, 4);
      const unsigned char* sym = sqlite3_column_text(stmt.handle, 5);
      t.buyer_id      = bid ? reinterpret_cast<const char*>(bid) : "";
      t.seller_id     = sid ? reinterpret_cast<const char*>(sid) : "";
      t.symbol        = sym ? reinterpret_cast<const char*>(sym) : "";
      t.qty           = sqlite3_column_double(stmt.handle, 6);
      t.price         = sqlite3_column_double(stmt.handle, 7);
      t.timestamp     = sqlite3_column_int64(stmt.handle, 8);
      trades.push_back(std::move(t));
    }
    return trades;
  }
}
