#pragma once

#include "database/db.h"
#include "models/order.h"
#include "models/trade.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace services
{
  enum class PlaceOrderStatus
  {
    ok,
    insufficient_funds,
    insufficient_holdings,
    invalid_input,
    error
  };

  enum class CancelOrderStatus
  {
      ok,
      not_found,
      not_owner,
      already_closed,
      error
  };

  struct PlaceOrderResult
  {
    PlaceOrderStatus        status{};
    std::optional<std::int64_t> order_id;
  };

  struct Position
  {
    std::string  symbol;
    double       qty{};
  };

  struct AccountView
  {
    std::string username;
    double      cash{};
    std::vector<Position> positions;
  };

  struct MarketAsset
  {
    std::string symbol;
    std::string name;
    std::string category;
    std::string icon;
    bool        is_new{};
    double      base{};
    double      price{};
    double      step{};
    double      volume_24h_usd{};
    double      volume_step_usd{};
  };

  class TradeService
  {
  public:
    explicit TradeService(database::Db& db);

    PlaceOrderResult           place_order(const std::string& user_id,
                                           const std::string& symbol,
                                           const std::string& side,
                                           double             price,
                                           double             qty);

    CancelOrderStatus          cancel_order(const std::string& user_id,
                                            std::int64_t       order_id);

    std::vector<models::Order> get_order_book(const std::string& symbol) const;
std::vector<models::Trade> get_trade_history(const std::string& symbol) const;
    std::vector<models::Order> get_user_orders(const std::string& username) const;
    std::optional<AccountView> get_account(const std::string& username) const;
    std::vector<Position>      get_positions(const std::string& username) const;
    std::vector<MarketAsset>   get_markets();

  private:
    database::Db& db_;
    std::mutex    mutex_;
    std::int64_t  seq_{};
std::int64_t  last_market_tick_{};
    std::uint32_t market_rng_{ 0xC0FFEEu };
    std::unordered_map<std::string, MarketAsset> markets_;

    void match_orders(const std::string& symbol);
  };
}
