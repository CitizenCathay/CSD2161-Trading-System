# Backend tests and benchmarks

Correctness tests and performance benchmarks for the matching engine and the HTTP layer.

These live outside `Backend/src` deliberately: `Backend/CMakeLists.txt` globs `src/*.cpp`
recursively into the `Backend` executable, so a second `main()` placed there would collide
with `Main.cpp`. Each target here compiles the production sources it exercises directly,
because `Backend` is an executable and cannot be linked against.

## Running

Configure and build as usual (`build.bat`, or CMake with the vcpkg toolchain), then:

```bash
ctest -C Debug --output-on-failure      # correctness tests
```

The benchmarks are deliberately not registered with CTest — they take minutes and measure
throughput, so run them on an otherwise idle machine:

```bash
./BenchMatching                          # matching engine: throughput, latency, book depth
./BenchHttp                              # HTTP layer: thread-per-client under concurrency
```

Both write their SQLite files into the working directory and clean up after themselves.

## What the tests cover

`test_matching.cpp` — 34 checks over the engine's semantics, each on a fresh in-memory
database:

| Test | Asserts |
|---|---|
| time priority | at equal price, the earliest `arrival` fills first |
| price priority | best-priced resting order fills first, at the resting order's price |
| partial fill | leftover quantity stays open with status `partial_fill` |
| no cross | a bid below the ask does not trade |
| balance checks | cannot buy beyond cash, cannot sell shares not held |
| validation | empty symbol, bad side, zero/negative price and quantity are rejected |
| cancel | a cancelled order never fills; double-cancel reports `already_closed` |
| conservation | buy-then-sell restores cash and holdings exactly |
| sequencing | `arrival` is strictly increasing |

These exist to guard performance work. They must produce identical output before and after
any change to the matching path.

## What the benchmarks measure

`bench_matching.cpp` runs four scenarios:

- **A** — on-disk SQLite (WAL, durable commits), single thread: end-to-end order cost as shipped
- **B** — same, with 1/2/4/8 threads on one shared `TradeService`: contention on the
  mutex-guarded critical section, plus a post-run audit (`trades == orders`,
  `filled == 2 x orders`) proving no fills were lost or duplicated
- **C** — in-memory SQLite: engine cost with disk sync removed
- **D** — in-memory, against a pre-seeded resting book: how per-order latency scales with
  open-order count

`bench_http.cpp` starts the real `HttpServer` on a background thread and drives it with a
load generator that opens one TCP connection per request, matching the server's
`Connection: close` thread-per-client design. It sweeps concurrency over a static route
(socket and routing overhead only) and over an order-placing route (full stack).

## Baseline results

Measured on one developer laptop, `/O2`, SQLite 3.45.3. Absolute numbers are
machine-specific; the ratios are the point.

`match_orders()` re-runs `get_open_orders()` after every fill. Before the index existed,
that was a full scan of the entire `orders` table — and because filled orders are never
archived, the scan grew with total history rather than with book depth, so per-order cost
degraded permanently as the system was used.

Adding `idx_orders_book` on `orders(symbol, status, side, price, arrival)`, and seeding the
market-maker balance once instead of rewriting it on every order:

| Scenario | Before | After | Gain |
|---|---|---|---|
| A — on-disk, 1 thread | 52.5 ord/s, p50 12.1 ms | 462.5 ord/s, p50 1.9 ms | 8.8x |
| B — 8 threads | 58.8 ord/s | 393.7 ord/s | 6.7x |
| C — in-memory, 1 thread | 172.0 ord/s, p50 4.9 ms | 4296.8 ord/s, p50 0.18 ms | 25x |
| D — resting book 2000 | 45.0 ord/s | 267.9 ord/s | 6.0x |

Worst-case latency in scenario A fell from 402 ms to 16.2 ms.

## Known remaining bottlenecks

- **Depth still costs.** Scenario D falls from 6468 to 268 ord/s as the book grows 0 to 2000.
  The `ORDER BY` spans two status values so SQLite still sorts, and `match_orders` still
  re-queries the whole book after every fill. Fixing this needs an in-memory order book
  (`std::map<price, std::deque<Order>>` per side) with SQLite as a write-behind journal.
- **Concurrency does not scale.** Throughput is flat from 1 to 8 threads (372 -> 394 ord/s):
  one global mutex and one SQLite connection mean threads queue rather than overlap. The
  8-thread worst case is still ~8 s, because the lock is held across disk I/O and
  `std::mutex` is unfair. The correctness guarantee holds; the scaling does not.
- **Commit fan-out.** A single order still costs several durable commits: the reserve
  transaction, the contra-order insert, and the settlement transaction are separate. Folding
  the order lifecycle into one transaction (via `SAVEPOINT`, since `match_orders` opens its
  own) would cut the remaining syncs and close a crash-consistency gap — a crash between the
  order insert and settlement currently leaves a resting contra order with no matching fill.
- **`PRAGMA synchronous`** is left at the default `FULL`. Pairing WAL with `NORMAL` is the
  usual production choice and is measurably faster, at the cost of losing recent commits on
  OS crash or power loss.
