# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

```bash
# Configure (Release is the default workflow)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=1

# Build everything
cmake --build build -j$(nproc)

# Build a specific target
cmake --build build --target unit_tests -j$(nproc)
cmake --build build --target bench_matching -j$(nproc)

# Run all tests
./bin/unit_tests

# Run a single test
./bin/unit_tests --gtest_filter='TestSuiteName.TestName'

# Admin UI (C++ backend first, then Python frontend)
./bin/admin_main &
cd admin && ./start.sh
```

- C++23, CMake 3.28+, requires `ninja-build`.
- FetchContent pulls `googletest` (v1.17.0) and `nlohmann/json` (v3.11.3).
- CI enforces clang-format (LLVM style, 4-space indent) and clang-tidy on PRs.
- Release builds use `-O3 -march=native -flto`. Benchmarks depend on these flags — changing them invalidates comparisons with `docs/benchmarks/` results.

## Architecture

The system can operate in two modes: **standalone exchange** (match orders locally) or **exchange pre-processor** (risk-check + internal match, then forward to external exchange). Both flows share the same core pipeline.

### Processing pipeline

```
Client / Strategy  -->  TradeSystem  -->  [Router by market+securityId hash]
                                            │
                          ┌─────────────────┼─────────────────┐
                     WorkerBucket-0    WorkerBucket-1 ...  WorkerBucket-N
                          │
                     SecurityCore  -->  RiskController  -->  MatchingEngine
                          │                                     │
                          └────────── TradeLogger ──────────────┘
```

**TradeSystem** (`include/trade_system.h`, `src/trade_system.cpp`) is the top-level orchestrator. It owns multiple `WorkerBucket`s, each with a dedicated thread and SPSC command queue. Orders are routed by hashing `market+securityId` to a bucket; each bucket processes its commands serially, eliminating lock contention within a security.

**SecurityCore** (`include/security_core.h`, `src/security_core.cpp`) chains RiskController → MatchingEngine. It also manages pending state for pre-exchange mode (orders forwarded to the external venue but not yet confirmed/cancelled).

**RiskController** (`include/risk_controller.h`, `src/risk_controller.cpp`) detects cross-trades (same shareholder + same security + opposite side with resting quantity) in O(1) via `shareholderId+market+securityId` composite key maps tracking aggregate buy/sell volumes.

**MatchingEngine** (`include/matching_engine.h`, `src/matching_engine.cpp`) uses a Limit Order Book design per `(market, securityId)`. Core data structures live in `include/lob.h`:
- `OrderPool` — pre-allocated object pool with freelist, O(1) alloc/free
- `PriceLevel` — intrusive doubly-linked list per price level, O(1) head/tail insert/remove
- `PriceBitSet` — bitmap with `__builtin_ctzll`/`clzll` for O(1) best-price scan and next-level traversal
- `OrderBook` — single-side (bid or ask) book combining the above three, with price discretization (tick=0.01) for O(1) level indexing
- `FastHashmap` (`include/fast_hashmap.h`) — Robin Hood open-addressing hash map for book lookup by `BookKey` and order reverse-index by `OrderId`

### Worker threading & core pinning

Each `WorkerBucket` has a dedicated thread. The `TradeSystem` constructor accepts a `std::vector<int>` of core IDs — each worker thread is pinned to its assigned core via `pthread_setaffinity_np` (`include/utils.h:pin_to_core`). Submit threads push commands to SPSC queues; bucket workers pop and process serially. Worker threads busy-wait on their SPSC queue (no blocking/notification) to minimize wake-up latency.

### Command flow (zero-heap types)

Command variants (`CmdOrder`, `CmdCancel`, `CmdResponse`, `CmdMarketData` — see `include/trade_system.h`) carry parsed structs through the SPSC queue. Producers (submit threads) parse JSON once into structs; workers consume the structs directly. JSON is only rebuilt at the boundary when forwarding to an external exchange or logging. This eliminates per-order heap allocation and parse/re-parse overhead in the hot path.

### Lock-free SPSC queue

`SpscQueue<T, N>` (`include/spsc.h`) is a pre-allocated lock-free ring buffer (power-of-2 capacity, default 4096). Each `WorkerBucket` has one. Producers (submit threads) push; the bucket's worker thread pops. Only single-producer, single-consumer is safe. Uses cached head/tail to reduce cache-coherency traffic.

### TSC measurement utilities

`include/utils.h` provides `rdtsc_lfence`, `rdtscp_lfence`, and `calibrate_tsc_ghz` for cycle-accurate latency measurement. Benchmarks under `benchmarks/latency/` use these to measure end-to-end latency in CPU cycles, converted to nanoseconds via the calibrated TSC frequency.

### Zero-heap data types

- `FixedStr<N>` (`include/fixed_str.h`) — stack-allocated string for IDs (OrderId, SecurityId, etc.). Avoids heap fragmentation on hot paths.
- `FastHashmap<K, V>` (`include/fast_hashmap.h`) — Robin Hood hash map with backward-shift deletion. Used for order book index and order reverse-lookup.

### Admin interface

`AdminServer` (`include/admin_server.h`, `src/admin_server.cpp`) is a TCP server (port 32000) using epoll. Python backend (`admin/server.py`, FastAPI + WebSocket) bridges to a Streamlit frontend (`admin/app.py`). Communication uses JSON Lines protocol.

### TradeLogger

`TradeLogger` (`include/trade_logger.h`, `src/trade_logger.cpp`) writes all trading events (order new/confirm/reject/execution/cancel, market data) as JSONL with millisecond timestamps via an async background writer thread.

### Outputs / executables

- `bin/` — all compiled binaries (set via `EXECUTABLE_OUTPUT_PATH`)
- `lib/` — `libtrade_engine.a`
- Throughput benchmarks: `benchmark` (single-thread full pipeline), `bench_matching` (matching-only), `bench_concurrent` (multi-thread contention), `bench_multicore` (multi-bucket scaling), `bench_network` (TCP I/O)
- Latency benchmarks (TSC, cycle-accurate): `latency_matching` (matching-only latency), `latency_multicore` (multi-bucket latency), `latency_e2e` (end-to-end: SPSC push → worker pop → dispatch → matching → callback)
- Tests live in `tests/`, benchmarks in `benchmarks/`, example programs in `examples/`
