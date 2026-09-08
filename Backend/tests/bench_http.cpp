// Benchmarks the real TCP/thread-per-client HTTP layer (server::HttpServer + server::Router),
// compiled directly against the repo's own http_server.cpp / router.cpp, plus the real
// matching engine (trade_service.cpp / db.cpp) wired behind a POST /orders route.
//
// Runs the actual accept()-loop server (one detached std::thread per connection, exactly
// as in http_server.cpp) on a background thread, then hammers it with a load generator that
// opens one real TCP connection per request (matching the server's "Connection: close" /
// thread-per-client design — no keep-alive), sweeping concurrency to see how request
// latency/throughput scale as more simultaneous client threads are added.

#define WIN32_LEAN_AND_MEAN
#include "server/http_server.h"
#include "server/router.h"
#include "database/db.h"
#include "services/trade_service.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

using clock_type = std::chrono::steady_clock;

namespace
{
  constexpr unsigned short kPort = 8099;

  struct RequestResult
  {
    bool   ok{};
    double ms{};
  };

  RequestResult do_request(const char* method, const char* path)
  {
    const auto t0 = clock_type::now();

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return { false, 0.0 };

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
      closesocket(s);
      return { false, 0.0 };
    }

    std::string req;
    req += method;
    req += ' ';
    req += path;
    req += " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";

    ::send(s, req.data(), static_cast<int>(req.size()), 0);

    char buf[4096];
    int received;
    bool got_any = false;
    while ((received = ::recv(s, buf, sizeof(buf), 0)) > 0) got_any = true;
    closesocket(s);

    const auto t1 = clock_type::now();
    return { got_any, std::chrono::duration<double, std::milli>(t1 - t0).count() };
  }

  struct SweepStats
  {
    long long count{};
    double wall_ms{};
    int errors{};
    std::vector<double> latencies_ms;
  };

  double percentile(const std::vector<double>& sorted_ms, double p)
  {
    if (sorted_ms.empty()) return 0.0;
    const size_t idx = static_cast<size_t>(p * static_cast<double>(sorted_ms.size() - 1));
    return sorted_ms[idx];
  }

  void run_sweep(const char* label, const char* method, const char* path, int concurrency, int per_client)
  {
    std::vector<std::thread> clients;
    std::vector<std::vector<double>> lat(concurrency);
    std::atomic<int> errors{ 0 };

    const auto t0 = clock_type::now();
    for (int c = 0; c < concurrency; ++c)
    {
      clients.emplace_back([&, c]()
      {
        lat[c].reserve(per_client);
        for (int i = 0; i < per_client; ++i)
        {
          const auto r = do_request(method, path);
          if (!r.ok) errors.fetch_add(1);
          else lat[c].push_back(r.ms);
        }
      });
    }
    for (auto& th : clients) th.join();
    const auto t1 = clock_type::now();

    std::vector<double> all;
    for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());

    const double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const long long n = static_cast<long long>(all.size());
    const double rps = wall_ms > 0.0 ? static_cast<double>(n) / (wall_ms / 1000.0) : 0.0;

    std::printf("%-10s conc=%-4d n=%-6lld wall=%9.1fms  throughput=%9.1f req/s  p50=%7.3fms  p95=%7.3fms  p99=%7.3fms  max=%8.3fms  errors=%d\n",
                label, concurrency, n, wall_ms, rps,
                percentile(all, 0.50), percentile(all, 0.95), percentile(all, 0.99),
                all.empty() ? 0.0 : all.back(), errors.load());
  }
}

int main()
{
  // Start from a clean DB so every benchmark user is fresh and clears the balance check.
  for (const char* suffix : { "", "-wal", "-shm", "-journal" })
    std::remove((std::string("bench_http.sqlite") + suffix).c_str());

  auto db_opt = database::Db::open("bench_http.sqlite");
  if (!db_opt) { std::fprintf(stderr, "failed to open db\n"); return 1; }
  services::TradeService svc(*db_opt);
  std::atomic<long long> order_seq{ 0 };

  server::Router router;

  static const char* kMarketsBody = R"({"symbol":"BTC","price":67245.00,"status":"ok"})";
  router.add_route("GET", "/markets", [](const server::HttpRequest&)
  {
    return server::HttpResponse{ 200, "application/json; charset=utf-8", kMarketsBody };
  });

  router.add_route("POST", "/orders", [&](const server::HttpRequest&) -> server::HttpResponse
  {
    const long long n = order_seq.fetch_add(1);
    const std::string user = "http_bench_u" + std::to_string(n);
    auto res = svc.place_order(user, "BTC", "buy", 67245.00, 1.0);
    if (res.status == services::PlaceOrderStatus::ok)
      return { 201, "application/json; charset=utf-8", R"({"status":"ok"})" };
    return { 400, "application/json; charset=utf-8", R"({"status":"error"})" };
  });

  server::HttpServer http_server(kPort);
  std::thread server_thread([&]() { http_server.run(router); });
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::fprintf(stderr, "client WSAStartup failed\n"); return 1; }

  std::printf("=== HTTP server benchmark (real HttpServer/Router, thread-per-client, over 127.0.0.1:%d) ===\n\n", kPort);

  std::printf("--- GET /markets (pure socket + routing overhead, no DB) ---\n");
  for (int C : { 1, 10, 50, 100, 200 })
    run_sweep("GET", "GET", "/markets", C, 30);

  std::printf("\n--- POST /orders (full stack: socket + parse + matching engine + SQLite commit) ---\n");
  for (int C : { 1, 10, 50, 100, 200 })
    run_sweep("POST", "POST", "/orders", C, 30);

  WSACleanup();
  std::fflush(stdout);
  std::_Exit(0); // HttpServer::run() never returns; terminate the whole process outright
}
