// Benchmarks the real matching-engine code path (services::TradeService::place_order),
// compiled directly against the repo's own trade_service.cpp / db.cpp / models.
//
// Scenarios
//   A) durable on-disk SQLite (exactly as db.cpp configures it: journal_mode=WAL,
//      default synchronous) single-threaded -> end-to-end order throughput/latency
//   B) same, with 1/2/4/8 threads sharing one TradeService -> real contention on the
//      mutex-guarded critical section, plus a correctness audit of the resulting DB
//   C) in-memory SQLite, single-threaded -> engine cost with disk fsync removed
//   D) in-memory SQLite, with a pre-seeded resting book -> how per-order latency scales
//      with open-order count (match_orders re-reads and re-sorts the book every pass)

#include "database/db.h"
#include "services/trade_service.h"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using clock_type = std::chrono::steady_clock;

namespace
{
  constexpr double kPrice = 67245.00;
  constexpr double kQty   = 1.0;

  struct RunStats
  {
    long long count{};
    double wall_ms{};
    std::vector<double> latencies_ms;
  };

  double percentile(const std::vector<double>& sorted_ms, double p)
  {
    if (sorted_ms.empty()) return 0.0;
    const size_t idx = static_cast<size_t>(p * static_cast<double>(sorted_ms.size() - 1));
    return sorted_ms[idx];
  }

  void print_stats(const char* label, RunStats& s)
  {
    std::sort(s.latencies_ms.begin(), s.latencies_ms.end());
    const double ops = s.wall_ms > 0.0 ? static_cast<double>(s.count) / (s.wall_ms / 1000.0) : 0.0;
    std::printf("%-26s n=%-6lld wall=%9.1fms  %9.1f ord/s  p50=%8.3fms  p95=%8.3fms  p99=%8.3fms  max=%9.3fms\n",
                label, s.count, s.wall_ms, ops,
                percentile(s.latencies_ms, 0.50), percentile(s.latencies_ms, 0.95),
                percentile(s.latencies_ms, 0.99), s.latencies_ms.empty() ? 0.0 : s.latencies_ms.back());
    std::fflush(stdout);
  }

  // NOTE: only call once every Db handle on the file is closed -- Windows refuses to
  // delete a file that is still open, which silently carries state between runs.
  void remove_db_files(const std::string& path)
  {
    const char* suffixes[] = { "", "-wal", "-shm", "-journal" };
    for (const char* suffix : suffixes)
    {
      const std::string file = path + suffix;
      std::remove(file.c_str());
      if (std::FILE* f = std::fopen(file.c_str(), "rb"))
      {
        std::fclose(f);
        std::fprintf(stderr, "FATAL: could not delete %s -- results would carry over\n", file.c_str());
        std::exit(2);
      }
    }
  }

  long long count_rows(sqlite3* h, const char* sql)
  {
    sqlite3_stmt* stmt = nullptr;
    long long result = -1;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
      if (sqlite3_step(stmt) == SQLITE_ROW) result = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return result;
  }

  // Places `n` orders from distinct fresh users (each starts with the default $100k, so a
  // single 1 x $67,245 buy always clears the balance check) and times each call.
  void place_n(services::TradeService& svc, const std::string& user_prefix, int n,
               RunStats& stats, std::atomic<int>& failures)
  {
    stats.latencies_ms.reserve(stats.latencies_ms.size() + n);
    for (int i = 0; i < n; ++i)
    {
      const std::string user = user_prefix + std::to_string(i);
      const auto t0 = clock_type::now();
      const auto res = svc.place_order(user, "BTC", "buy", kPrice, kQty);
      const auto t1 = clock_type::now();
      if (res.status != services::PlaceOrderStatus::ok) failures.fetch_add(1);
      stats.latencies_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
  }

  void audit(sqlite3* h, long long expected_orders, int failures)
  {
    const long long trades = count_rows(h, "SELECT COUNT(*) FROM trades;");
    const long long filled = count_rows(h, "SELECT COUNT(*) FROM orders WHERE status='filled';");
    const bool pass = (failures == 0) && (trades == expected_orders) && (filled == expected_orders * 2);
    std::printf("  audit: rejected=%d  trades=%lld/%lld  filled_orders=%lld/%lld  -> %s\n\n",
                failures, trades, expected_orders, filled, expected_orders * 2,
                pass ? "PASS (no lost or duplicated fills)" : "FAIL");
    std::fflush(stdout);
  }
}

int main()
{
  const std::string db_path = "bench_matching.sqlite";
  remove_db_files(db_path);

  std::printf("=== Matching engine benchmark ===\n");
  std::printf("Built from the repo's own trade_service.cpp / db.cpp, /O2, SQLite %s\n\n", sqlite3_libversion());

  // ---------- A) durable on-disk, single thread ----------
  std::printf("--- A) on-disk SQLite (WAL, durable commits), single thread ---\n");
  {
    const int N = 2000;
    {
      auto db = database::Db::open(db_path);
      if (!db) { std::fprintf(stderr, "open failed\n"); return 1; }
      services::TradeService svc(*db);

      std::atomic<int> warm{ 0 };
      RunStats warmup;
      place_n(svc, "warm_", 50, warmup, warm);   // let WAL/page cache settle before timing

      RunStats stats;
      std::atomic<int> failures{ 0 };
      const auto t0 = clock_type::now();
      place_n(svc, "diskA_", N, stats, failures);
      const auto t1 = clock_type::now();
      stats.count = N;
      stats.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

      print_stats("single thread", stats);
      audit(db->handle(), N + 50, failures.load() + warm.load());
    }
    remove_db_files(db_path);
  }

  // ---------- B) durable on-disk, concurrent callers ----------
  std::printf("--- B) on-disk SQLite, concurrent callers on one shared TradeService ---\n");
  for (int T : { 1, 2, 4, 8 })
  {
    const int per_thread = 500;
    {
      auto db = database::Db::open(db_path);
      if (!db) { std::fprintf(stderr, "open failed\n"); return 1; }
      services::TradeService svc(*db);

      std::vector<RunStats> per_thread_stats(T);
      std::vector<std::thread> threads;
      std::atomic<int> failures{ 0 };

      const auto t0 = clock_type::now();
      for (int t = 0; t < T; ++t)
      {
        threads.emplace_back([&, t]()
        {
          const std::string prefix = "T" + std::to_string(T) + "_t" + std::to_string(t) + "_u";
          place_n(svc, prefix, per_thread, per_thread_stats[t], failures);
        });
      }
      for (auto& th : threads) th.join();
      const auto t1 = clock_type::now();

      RunStats combined;
      for (auto& s : per_thread_stats)
        combined.latencies_ms.insert(combined.latencies_ms.end(), s.latencies_ms.begin(), s.latencies_ms.end());
      combined.count = static_cast<long long>(T) * per_thread;
      combined.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

      char label[64];
      std::snprintf(label, sizeof(label), "%d thread%s", T, T == 1 ? "" : "s");
      print_stats(label, combined);
      audit(db->handle(), combined.count, failures.load());
    }
    remove_db_files(db_path);
  }

  // ---------- C) in-memory, single thread (engine cost without fsync) ----------
  std::printf("--- C) in-memory SQLite, single thread (isolates engine from disk sync) ---\n");
  {
    const int N = 5000;
    auto db = database::Db::open(":memory:");
    if (!db) { std::fprintf(stderr, "open failed\n"); return 1; }
    services::TradeService svc(*db);

    RunStats warmup;
    std::atomic<int> warm{ 0 };
    place_n(svc, "warm_", 100, warmup, warm);

    RunStats stats;
    std::atomic<int> failures{ 0 };
    const auto t0 = clock_type::now();
    place_n(svc, "memC_", N, stats, failures);
    const auto t1 = clock_type::now();
    stats.count = N;
    stats.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    print_stats("single thread", stats);
    audit(db->handle(), N + 100, failures.load() + warm.load());
  }

  // ---------- D) in-memory, latency vs resting book depth ----------
  std::printf("--- D) in-memory SQLite, per-order latency vs resting open-order count ---\n");
  for (int depth : { 0, 100, 500, 1000, 2000 })
  {
    auto db = database::Db::open(":memory:");
    if (!db) { std::fprintf(stderr, "open failed\n"); return 1; }

    // Seed `depth` resting buy orders priced far below the trading price so they never
    // cross, and insert them before TradeService is constructed so seq_ starts above them.
    for (int i = 0; i < depth; ++i)
    {
      models::Order resting;
      resting.user_id       = "__depth__";
      resting.symbol        = "BTC";
      resting.side          = models::Side::Buy;
      resting.price         = 1000.0 + i;
      resting.qty_total     = 1.0;
      resting.qty_remaining = 1.0;
      resting.arrival       = i + 1;
      resting.status        = models::OrderStatus::Open;
      db->insert_order(resting);
    }

    services::TradeService svc(*db);
    const int N = 300;
    RunStats stats;
    std::atomic<int> failures{ 0 };

    const auto t0 = clock_type::now();
    place_n(svc, "depth_", N, stats, failures);
    const auto t1 = clock_type::now();
    stats.count = N;
    stats.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    char label[64];
    std::snprintf(label, sizeof(label), "resting book = %d", depth);
    print_stats(label, stats);
    if (failures.load() != 0) std::printf("  (rejected=%d)\n", failures.load());
  }

  std::printf("\ndone\n");
  return 0;
}
