// Semantic correctness tests for the matching engine, run against the repo's own
// trade_service.cpp / db.cpp. Each test gets a fresh in-memory database.
//
// These exist to guard a performance refactor: they must pass identically before and
// after. They check matching *semantics* (price priority, time priority, the execution
// price rule, partial fills, balance checks, cancellation, value conservation), not speed.

#include "database/db.h"
#include "services/trade_service.h"

#include <sqlite3.h>

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace
{
  int g_failures = 0;
  int g_checks = 0;

  const std::string kMarket = "__market__";

  void check(bool cond, const std::string& what)
  {
    ++g_checks;
    if (!cond)
    {
      ++g_failures;
      std::printf("    FAIL: %s\n", what.c_str());
    }
  }

  void check_near(double got, double want, const std::string& what)
  {
    ++g_checks;
    if (std::fabs(got - want) > 1e-6)
    {
      ++g_failures;
      std::printf("    FAIL: %s (got %.6f, want %.6f)\n", what.c_str(), got, want);
    }
  }

  struct Fixture
  {
    std::optional<database::Db> db;
    std::optional<services::TradeService> svc;

    Fixture()
    {
      db = database::Db::open(":memory:");
      // Give the market maker inventory so it can rest sell orders.
      db->upsert_holdings(kMarket, "BTC", 1000.0);
      db->upsert_account(kMarket, 1.0e12);
      svc.emplace(*db);
    }
  };

  std::vector<models::Trade> trades_of(database::Db& db)
  {
    return db.get_trade_history("BTC");
  }

  // ---------------------------------------------------------------------------

  void test_time_priority()
  {
    std::printf("  time priority: equal price -> earliest arrival fills first\n");
    Fixture f;

    const auto first  = f.svc->place_order(kMarket, "BTC", "sell", 100.0, 1.0);
    const auto second = f.svc->place_order(kMarket, "BTC", "sell", 100.0, 1.0);
    check(first.status == services::PlaceOrderStatus::ok, "first resting sell accepted");
    check(second.status == services::PlaceOrderStatus::ok, "second resting sell accepted");

    const auto buy = f.svc->place_order("alice", "BTC", "buy", 100.0, 1.0);
    check(buy.status == services::PlaceOrderStatus::ok, "buy accepted");

    const auto t = trades_of(*f.db);
    check(t.size() == 1, "exactly one trade");
    if (!t.empty())
      check(t[0].sell_order_id == *first.order_id, "earliest-arrival sell filled, not the later one");
  }

  void test_price_priority_and_execution_price()
  {
    std::printf("  price priority: best price fills first, at the resting order's price\n");
    Fixture f;

    const auto cheap = f.svc->place_order(kMarket, "BTC", "sell", 90.0, 1.0);
    const auto dear  = f.svc->place_order(kMarket, "BTC", "sell", 100.0, 1.0);
    check(cheap.status == services::PlaceOrderStatus::ok, "90 sell accepted");
    check(dear.status == services::PlaceOrderStatus::ok, "100 sell accepted");

    f.svc->place_order("alice", "BTC", "buy", 100.0, 1.0);

    const auto t = trades_of(*f.db);
    check(t.size() == 1, "exactly one trade");
    if (!t.empty())
    {
      check(t[0].sell_order_id == *cheap.order_id, "lowest-priced sell filled");
      check_near(t[0].price, 90.0, "executed at the resting sell's price");
    }
  }

  void test_partial_fill()
  {
    std::printf("  partial fill: leftover quantity stays open\n");
    Fixture f;

    const auto resting = f.svc->place_order(kMarket, "BTC", "sell", 100.0, 5.0);
    f.svc->place_order("alice", "BTC", "buy", 100.0, 2.0);

    const auto t = trades_of(*f.db);
    check(t.size() == 1, "exactly one trade");
    if (!t.empty()) check_near(t[0].qty, 2.0, "traded quantity is the smaller side");

    const auto after = f.db->get_order_by_id(*resting.order_id);
    check(after.has_value(), "resting order still present");
    if (after)
    {
      check_near(after->qty_remaining, 3.0, "remaining quantity");
      check(after->status == models::OrderStatus::PartialFill, "status is partial_fill");
    }
  }

  void test_no_cross_no_trade()
  {
    std::printf("  no cross: bid below ask does not trade\n");
    Fixture f;

    f.svc->place_order(kMarket, "BTC", "sell", 200.0, 1.0);
    f.svc->place_order("alice", "BTC", "buy", 100.0, 1.0);

    // alice's own contra sell is priced at 100 and will match her buy; the 200 ask must not.
    const auto t = trades_of(*f.db);
    for (const auto& trade : t)
      check(trade.price <= 100.0 + 1e-9, "no trade executed above the bid");
  }

  void test_insufficient_funds()
  {
    std::printf("  balance check: cannot buy beyond cash\n");
    Fixture f;

    const auto res = f.svc->place_order("poor", "BTC", "buy", 500000.0, 1.0);
    check(res.status == services::PlaceOrderStatus::insufficient_funds, "rejected as insufficient_funds");
    check(trades_of(*f.db).empty(), "no trade recorded");
  }

  void test_insufficient_holdings()
  {
    std::printf("  balance check: cannot sell shares not held\n");
    Fixture f;

    const auto res = f.svc->place_order("empty", "BTC", "sell", 100.0, 1.0);
    check(res.status == services::PlaceOrderStatus::insufficient_holdings, "rejected as insufficient_holdings");
    check(trades_of(*f.db).empty(), "no trade recorded");
  }

  void test_invalid_input()
  {
    std::printf("  validation: malformed orders rejected\n");
    Fixture f;

    check(f.svc->place_order("a", "",    "buy",  100.0,  1.0).status == services::PlaceOrderStatus::invalid_input, "empty symbol");
    check(f.svc->place_order("a", "BTC", "hold", 100.0,  1.0).status == services::PlaceOrderStatus::invalid_input, "bad side");
    check(f.svc->place_order("a", "BTC", "buy",    0.0,  1.0).status == services::PlaceOrderStatus::invalid_input, "zero price");
    check(f.svc->place_order("a", "BTC", "buy",  100.0,  0.0).status == services::PlaceOrderStatus::invalid_input, "zero quantity");
    check(f.svc->place_order("a", "BTC", "buy",  100.0, -5.0).status == services::PlaceOrderStatus::invalid_input, "negative quantity");
  }

  void test_cancel_removes_from_book()
  {
    std::printf("  cancel: a cancelled order no longer matches\n");
    Fixture f;

    const auto resting = f.svc->place_order(kMarket, "BTC", "sell", 90.0, 1.0);
    const auto cancelled = f.svc->cancel_order(kMarket, *resting.order_id);
    check(cancelled == services::CancelOrderStatus::ok, "cancel accepted");

    const auto again = f.svc->cancel_order(kMarket, *resting.order_id);
    check(again == services::CancelOrderStatus::already_closed, "second cancel reports already_closed");

    const auto wrong_owner = f.svc->cancel_order("mallory", 999999);
    check(wrong_owner == services::CancelOrderStatus::not_found, "unknown order reports not_found");

    f.svc->place_order("alice", "BTC", "buy", 100.0, 1.0);
    for (const auto& t : trades_of(*f.db))
      check(t.sell_order_id != *resting.order_id, "cancelled order never filled");
  }

  void test_value_conservation_round_trip()
  {
    std::printf("  conservation: buy then sell restores cash and holdings\n");
    Fixture f;

    const double start_cash = 100000.0;
    f.svc->place_order("bob", "BTC", "buy", 100.0, 1.0);

    check_near(f.db->get_cash("bob").value_or(-1.0), start_cash - 100.0, "cash reduced by trade value");
    check_near(f.db->get_holdings("bob", "BTC").value_or(-1.0), 1.0, "holding credited");

    f.svc->place_order("bob", "BTC", "sell", 100.0, 1.0);

    check_near(f.db->get_cash("bob").value_or(-1.0), start_cash, "cash restored after selling back");
    check_near(f.db->get_holdings("bob", "BTC").value_or(-1.0), 0.0, "holding returned to zero");
  }

  void test_arrival_sequence_monotonic()
  {
    std::printf("  sequencing: arrival numbers are strictly increasing\n");
    Fixture f;

    const auto a = f.svc->place_order(kMarket, "BTC", "sell", 100.0, 1.0);
    const auto b = f.svc->place_order(kMarket, "BTC", "sell", 100.0, 1.0);

    const auto oa = f.db->get_order_by_id(*a.order_id);
    const auto ob = f.db->get_order_by_id(*b.order_id);
    check(oa && ob && ob->arrival > oa->arrival, "later order has a higher arrival sequence");
  }
}

int main()
{
  std::printf("=== Matching engine correctness tests (SQLite %s) ===\n\n", sqlite3_libversion());

  test_time_priority();
  test_price_priority_and_execution_price();
  test_partial_fill();
  test_no_cross_no_trade();
  test_insufficient_funds();
  test_insufficient_holdings();
  test_invalid_input();
  test_cancel_removes_from_book();
  test_value_conservation_round_trip();
  test_arrival_sequence_monotonic();

  std::printf("\n%d checks, %d failures -> %s\n",
              g_checks, g_failures, g_failures == 0 ? "ALL PASS" : "FAILURES");
  return g_failures == 0 ? 0 : 1;
}
