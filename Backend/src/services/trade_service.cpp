#include "services/trade_service.h"

#include "models/order.h"
#include "models/trade.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace services
{
  static std::int64_t now_seconds()
  {
    return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  }

  static std::uint32_t xorshift32(std::uint32_t& state)
  {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }

  static double rand_unit(std::uint32_t& state)
  {
    const auto v = xorshift32(state);
    return static_cast<double>(v) / 4294967296.0;
  }

  static services::MarketAsset make_asset(std::string symbol, std::string name, std::string category, std::string icon, bool is_new, double base, double step, double volume_step_usd)
  {
    services::MarketAsset a;
    a.symbol   = std::move(symbol);
    a.name     = std::move(name);
    a.category = std::move(category);
    a.icon     = std::move(icon);
    a.is_new   = is_new;
    a.base     = base;
    a.price    = base;
    a.step     = step;
    a.volume_step_usd = volume_step_usd;
    a.volume_24h_usd  = volume_step_usd * 400.0;
    return a;
  }

  // Initialises seq_ from DB to preserve arrival priority across restarts
  TradeService::TradeService(database::Db& db) : db_(db), seq_(db.get_max_arrival())
  {
    markets_.emplace("BTC",     make_asset("BTC",     "Bitcoin",           "crypto",      "bitcoin",     false, 67245.00, 0.002, 5500000.0));
    markets_.emplace("ETH",     make_asset("ETH",     "Ethereum",          "crypto",      "hexagon",     false,  3456.12, 0.003, 3200000.0));
    markets_.emplace("SOL",     make_asset("SOL",     "Solana",            "crypto",      "zap",         false,   142.85, 0.006, 1200000.0));
    markets_.emplace("ADA",     make_asset("ADA",     "Cardano",           "crypto",      "circle",      false,     0.485, 0.010,  650000.0));
    markets_.emplace("AAPL",    make_asset("AAPL",    "Apple Inc.",        "stocks",      "smartphone",  false,   178.35, 0.0015, 1800000.0));
    markets_.emplace("TSLA",    make_asset("TSLA",    "Tesla Inc.",        "stocks",      "car",         false,   242.15, 0.004,  2100000.0));
    markets_.emplace("EUR/USD", make_asset("EUR/USD", "Euro / US Dollar",  "forex",       "dollar-sign", false,     1.0845, 0.0008, 9000000.0));
    markets_.emplace("XAU/USD", make_asset("XAU/USD", "Gold",              "commodities", "gem",         false,  2045.80, 0.0010, 4200000.0));

    markets_.emplace("IMX",     make_asset("IMX",     "Immutable X",       "crypto",      "leaf",        true,      2.150, 0.012,  300000.0));
    markets_.emplace("RON",     make_asset("RON",     "Ronin",             "crypto",      "shield",      true,      1.780, 0.014,  260000.0));
    markets_.emplace("STRK",    make_asset("STRK",    "Starknet",          "crypto",      "layers",      true,      1.250, 0.016,  280000.0));
    last_market_tick_ = now_seconds();
  }

  // -----------------------------------------------------------------------
  // place_order
  //   Validates the user can afford/cover the order, inserts it, then
  //   runs the matching loop.  The mutex serialises all of this so two
  //   concurrent POST /orders calls cannot race on the book.
  // -----------------------------------------------------------------------
  PlaceOrderResult TradeService::place_order(
    const std::string& user_id,
    const std::string& symbol,
    const std::string& side,
    double             price,
    double             qty)
  {
    if (symbol.empty() || (side != "buy" && side != "sell") || price <= 0.0 || qty <= 0)
      return { PlaceOrderStatus::invalid_input, std::nullopt };

    std::lock_guard<std::mutex> lock(mutex_);

    const std::string market_maker = "__market__";

    const auto cash_opt = db_.get_cash(user_id);
    const double cash = cash_opt.value_or(100000.0);
    if (!cash_opt)
      db_.upsert_account_with_reserved(user_id, cash, 0.0);
    const double reserved_cash = db_.get_reserved_cash(user_id).value_or(0.0);

    if (side == "buy")
    {
      const double cost = price * static_cast<double>(qty);
      const double available_cash = cash - reserved_cash;
      if (available_cash < cost)
        return { PlaceOrderStatus::insufficient_funds, std::nullopt };
    }
    else
    {
      // Seller must actually hold enough shares
      const auto held = db_.get_holdings(user_id, symbol);
      const double holdings = held.value_or(0.0);
      if (holdings < qty)
        return { PlaceOrderStatus::insufficient_holdings, std::nullopt };
    }

    models::Order order;
    order.user_id       = user_id;
    order.symbol        = symbol;
    order.side          = (side == "buy") ? models::Side::Buy : models::Side::Sell;
    order.price         = price;
    order.qty_total     = qty;
    order.qty_remaining = qty;
    order.arrival       = ++seq_;   // monotonic sequence — determines resting priority
    order.status        = models::OrderStatus::Open;

    bool reserved_ok = true;
    if (side == "buy")
    {
      db_.begin_transaction();
      const double cost = price * static_cast<double>(qty);
      reserved_ok = db_.upsert_account_with_reserved(user_id, cash, reserved_cash + cost);
      if (!reserved_ok)
      {
        db_.rollback();
        return { PlaceOrderStatus::error, std::nullopt };
      }
    }

    const auto id = db_.insert_order(order);
    if (!id)
    {
      if (side == "buy")
        db_.rollback();
      return { PlaceOrderStatus::error, std::nullopt };
    }

    if (side == "buy")
      db_.commit();

    order.order_id = *id;

    if (user_id != market_maker)
    {
      db_.upsert_account(market_maker, 1.0e12);
      const auto mm_held = db_.get_holdings(market_maker, symbol).value_or(0.0);
      if (mm_held < qty)
      {
        db_.upsert_holdings(market_maker, symbol, 1000000000.0);
      }

      models::Order contra;
      contra.user_id       = market_maker;
      contra.symbol        = symbol;
      contra.side          = (side == "buy") ? models::Side::Sell : models::Side::Buy;
      contra.price         = price;
      contra.qty_total     = qty;
      contra.qty_remaining = qty;
      contra.arrival       = ++seq_;
      contra.status        = models::OrderStatus::Open;
      db_.insert_order(contra);
    }

    match_orders(symbol);

    return { PlaceOrderStatus::ok, *id };
  }

  std::vector<MarketAsset> TradeService::get_markets()
  {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto now = now_seconds();
    auto ticks = now - last_market_tick_;
    if (ticks < 0) ticks = 0;
    if (ticks > 10) ticks = 10;

    for (std::int64_t t = 0; t < ticks; ++t)
    {
      for (auto& [sym, a] : markets_)
      {
        const double prev = a.price;
        const double scale = a.base * a.step;
        const double delta = (rand_unit(market_rng_) - 0.5) * scale;
        const double floor_price = std::max(0.0001, a.base * 0.05);
        a.price = std::max(floor_price, a.price + delta);
        const double vol_add = (0.5 + rand_unit(market_rng_)) * a.volume_step_usd;
        a.volume_24h_usd = std::max(0.0, a.volume_24h_usd * 0.9995 + vol_add + (std::abs(a.price - prev) / std::max(0.0001, a.base)) * a.volume_step_usd);
      }
    }
    last_market_tick_ = now;

    std::vector<MarketAsset> out;
    out.reserve(markets_.size());
    for (const auto& [sym, a] : markets_) out.push_back(a);
    std::sort(out.begin(), out.end(), [](const MarketAsset& l, const MarketAsset& r) { return l.symbol < r.symbol; });
    return out;
  }

  // -----------------------------------------------------------------------
  // match_orders — implements the PDF algorithm exactly
  //   MATCH_ORDERS(book):
  //     while buy_book != empty and sell_book != empty
  //       best_buy  = highest price buy,  earliest arrival on tie
  //       best_sell = lowest  price sell, earliest arrival on tie
  //       if best_buy.price < best_sell.price → return
  //       determine trade price (resting-order rule / midpoint fallback)
  //       execute trade
  // -----------------------------------------------------------------------
  void TradeService::match_orders(const std::string& symbol)
  {
    constexpr double eps = 1e-9;
    while (true)
    {
      auto open = db_.get_open_orders(symbol);

      // Split into buy / sell and sort correctly
      std::vector<models::Order> buys, sells;
      for (auto& o : open)
      {
        if (o.side == models::Side::Buy)  buys.push_back(o);
        else                              sells.push_back(o);
      }

      // buys:  highest price first, then earliest arrival
      std::sort(buys.begin(), buys.end(), [](const models::Order& a, const models::Order& b) {
        if (a.price != b.price) return a.price > b.price;
        return a.arrival < b.arrival;
      });
      // sells: lowest price first, then earliest arrival
      std::sort(sells.begin(), sells.end(), [](const models::Order& a, const models::Order& b) {
        if (a.price != b.price) return a.price < b.price;
        return a.arrival < b.arrival;
      });

      if (buys.empty() || sells.empty()) break;

      models::Order& best_buy  = buys.front();
      models::Order& best_sell = sells.front();

      // No overlap so nothing to match
      if (best_buy.price < best_sell.price) break;

      // Resting-order price rule (PDF "Execution Price Rule")
      double trade_price{};
      if (best_buy.arrival < best_sell.arrival)
        trade_price = best_buy.price;          // buy was resting first
      else if (best_sell.arrival < best_buy.arrival)
        trade_price = best_sell.price;         // sell was resting first
      else
        trade_price = (best_buy.price + best_sell.price) / 2.0;  // midpoint fallback

      const double qty = std::min(best_buy.qty_remaining, best_sell.qty_remaining);
      if (qty <= eps) break;

      /*
      Solvency check: cash is only deducted when a trade executes, not when order is placed
      This means a buyer can place multiple open buy orders that individually pass validation 
      but collectively exceed their balance

      Example:
        * A has $100k; places buy A (600 @ $100 = $60k) -> passes validation
        * A places buy B (600 @ $100 = $60k) -> also passes (DB cash still $100k)
        * Seller arrives; order A matches: cash $100k -> $40k (ok)
        * Order B matches: cash $40k -> -$20k (negative balance)
      */
      {
          const double buyer_cash = db_.get_cash(best_buy.user_id).value_or(0.0);
		  // Re-check buyer's current cash before executing each trade; if insufficient, 
          // cancel the buy order and skip to the next best buy
          if (buyer_cash < trade_price * qty)
          {
              best_buy.status = models::OrderStatus::Cancelled;
              const double buyer_reserved = db_.get_reserved_cash(best_buy.user_id).value_or(0.0);
              const double release = best_buy.price * best_buy.qty_remaining;
              const double new_reserved = std::max(0.0, buyer_reserved - release);
              db_.upsert_account_with_reserved(best_buy.user_id, buyer_cash, new_reserved);
              db_.update_order(best_buy);
              buys.erase(buys.begin());
              continue;
          }
      }

      // Wrap all DB writes for this trade in a transaction; either everything
      // commits or nothing does; if any write fails we rollback and abort
      db_.begin_transaction();

      bool ok = true;

      // ---- update buyer account ----
      {
        const double buyer_cash = db_.get_cash(best_buy.user_id).value_or(0.0);
        const double buyer_reserved = db_.get_reserved_cash(best_buy.user_id).value_or(0.0);
        const double reserved_release = best_buy.price * qty;
        const double new_reserved = std::max(0.0, buyer_reserved - reserved_release);
        ok &= db_.upsert_account_with_reserved(best_buy.user_id, buyer_cash - trade_price * qty, new_reserved);

        const double buyer_held = db_.get_holdings(best_buy.user_id, symbol).value_or(0.0);
        ok &= db_.upsert_holdings(best_buy.user_id, symbol, buyer_held + qty);
      }

      // ---- update seller account ----
      {
        const double seller_cash = db_.get_cash(best_sell.user_id).value_or(0.0);
        ok &= db_.upsert_account(best_sell.user_id, seller_cash + trade_price * qty);

        const double seller_held = db_.get_holdings(best_sell.user_id, symbol).value_or(0.0);
        ok &= db_.upsert_holdings(best_sell.user_id, symbol, seller_held - qty);
      }

      // ---- record the trade ----
      models::Trade trade;
      trade.buy_order_id  = best_buy.order_id;
      trade.sell_order_id = best_sell.order_id;
      trade.buyer_id      = best_buy.user_id;
      trade.seller_id     = best_sell.user_id;
      trade.symbol        = symbol;
      trade.qty           = qty;
      trade.price         = trade_price;
      trade.timestamp     = now_seconds();
      ok &= db_.insert_trade(trade);

      // ---- update order quantities and statuses ----
      best_buy.qty_remaining  -= qty;
      best_sell.qty_remaining -= qty;

      if (best_buy.qty_remaining <= eps) best_buy.qty_remaining = 0.0;
      if (best_sell.qty_remaining <= eps) best_sell.qty_remaining = 0.0;

      best_buy.status  = (best_buy.qty_remaining  <= eps) ? models::OrderStatus::Filled : models::OrderStatus::PartialFill;
      best_sell.status = (best_sell.qty_remaining <= eps) ? models::OrderStatus::Filled : models::OrderStatus::PartialFill;

      ok &= db_.update_order(best_buy);
      ok &= db_.update_order(best_sell);

      if (!ok)
      {
        db_.rollback();
        break;
      }

      db_.commit();                 // All 7 writes succeed together

      // If neither was fully filled we are stuck, break to avoid infinite loop
      if (best_buy.qty_remaining > eps && best_sell.qty_remaining > eps) break;
    }
  }

  // Cancels an open or partially-filled order that belongs to the user
  CancelOrderStatus TradeService::cancel_order(const std::string& user_id, std::int64_t order_id)
  {
	  // Same mutex used in place_order; prevents race condition between matching and cancelling
      std::lock_guard<std::mutex> lock(mutex_);
      const auto check = db_.check_cancel_order(order_id, user_id);
      switch (check)
      {
      case database::CancelOrderCheck::ok:
      {
        const auto order = db_.get_order_by_id(order_id);
        if (!order) return CancelOrderStatus::error;

        db_.begin_transaction();
        bool ok = true;
        if (order->side == models::Side::Buy)
        {
          const double buyer_cash = db_.get_cash(order->user_id).value_or(0.0);
          const double buyer_reserved = db_.get_reserved_cash(order->user_id).value_or(0.0);
          const double release = order->price * order->qty_remaining;
          const double new_reserved = std::max(0.0, buyer_reserved - release);
          ok &= db_.upsert_account_with_reserved(order->user_id, buyer_cash, new_reserved);
        }
        ok &= db_.cancel_order(order_id, user_id);

        if (!ok)
        {
          db_.rollback();
          return CancelOrderStatus::error;
        }
        db_.commit();
        return CancelOrderStatus::ok;
      }
      case database::CancelOrderCheck::not_found:
        return CancelOrderStatus::not_found;
      case database::CancelOrderCheck::not_owner:
        return CancelOrderStatus::not_owner;
      case database::CancelOrderCheck::already_closed:
        return CancelOrderStatus::already_closed;
      default:
        return CancelOrderStatus::error;
      }
  }

  // -----------------------------------------------------------------------
  // read-only queries
  // -----------------------------------------------------------------------
  std::vector<models::Order> TradeService::get_order_book(const std::string& symbol) const
  {
    return db_.get_open_orders(symbol);
  }

  std::vector<models::Trade> TradeService::get_trade_history(const std::string& symbol) const
  {
    return db_.get_trade_history(symbol);
  }

  std::vector<models::Order> TradeService::get_user_orders(const std::string& username) const
  {
    return db_.get_orders_by_user(username);
  }

  std::optional<AccountView> TradeService::get_account(const std::string& username) const
  {
    const auto cash = db_.get_cash(username);
    if (!cash) return std::nullopt;
    AccountView av;
    av.username = username;
    av.cash     = *cash;
    for (const auto& [sym, qty] : db_.list_holdings(username))
      av.positions.push_back({ sym, qty });
    return av;
  }

  std::vector<Position> TradeService::get_positions(const std::string& username) const
  {
    std::vector<Position> out;
    const auto rows = db_.list_holdings(username);
    out.reserve(rows.size());
    for (const auto& [sym, qty] : rows)
    {
      Position p;
      p.symbol = sym;
      p.qty    = qty;
      out.push_back(std::move(p));
    }
    return out;
  }
}
