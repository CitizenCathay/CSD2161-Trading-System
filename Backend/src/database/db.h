#pragma once

#include "models/order.h"
#include "models/trade.h"
#include "models/user.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace database
{
  enum class CancelOrderCheck
  {
    ok,
    not_found,
    not_owner,
    already_closed,
    error
  };

  struct UserSummary
  {
    std::string  username;
    std::int64_t created_at{};
  };

  enum class InsertUserResult
  {
    ok,
    exists,
    error
  };

  std::string default_db_path();

  class Db
  {
  public:
    Db() = default;
    ~Db();
    Db(Db&& other) noexcept;
    Db& operator=(Db&& other) noexcept;

    Db(const Db&)            = delete;
    Db& operator=(const Db&) = delete;

    static std::optional<Db> open(const std::string& path);

    sqlite3* handle() const;

    // ---- transactions ----
    bool begin_transaction() const;
    bool commit() const;
    bool rollback() const;

    // ---- users ----
    std::optional<models::UserRecord>      get_user(const std::string& username) const;
    std::optional<bool>                    user_exists(const std::string& username) const;
    InsertUserResult                       insert_user(const std::string& username, const models::UserRecord& record, std::int64_t created_at) const;
    std::optional<std::vector<UserSummary>> list_users() const;

    // ---- accounts ----
    std::optional<double> get_cash(const std::string& username) const;
    bool                  upsert_account(const std::string& username, double cash) const;
    std::optional<double> get_reserved_cash(const std::string& username) const;
    bool                  upsert_account_with_reserved(const std::string& username, double cash, double reserved_cash) const;
    std::optional<double>       get_holdings(const std::string& username, const std::string& symbol) const;
    bool                        upsert_holdings(const std::string& username, const std::string& symbol, double qty) const;
    std::vector<std::pair<std::string, double>> list_holdings(const std::string& username) const;

    // ---- orders ----
    std::optional<std::int64_t>        insert_order(const models::Order& order) const;
    bool                               update_order(const models::Order& order) const;
    bool                               cancel_order(std::int64_t order_id, const std::string& user_id) const;
    CancelOrderCheck                   check_cancel_order(std::int64_t order_id, const std::string& user_id) const;
    std::optional<models::Order>       get_order_by_id(std::int64_t order_id) const;
    std::int64_t                       get_max_arrival() const;
    std::vector<models::Order>         get_open_orders(const std::string& symbol) const;
    std::vector<models::Order>         get_orders_by_user(const std::string& username) const;

    // ---- trades ----
    bool                               insert_trade(const models::Trade& trade) const;
    std::vector<models::Trade>         get_trade_history(const std::string& symbol) const;
  private:
    explicit Db(sqlite3* db);
    void close();

    sqlite3* db_{};
  };
}
