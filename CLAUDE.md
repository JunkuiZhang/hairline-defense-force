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

**MatchingEngine** (`include/matching_engine.h`, `src/matching_engine.cpp`) uses a Limit Order Book design per `(market, securityId)`:
- `OrderPool` — pre-allocated object pool with freelist, O(1) alloc/free
- `PriceLevel` — intrusive doubly-linked list per price level, O(1) head/tail insert/remove
- `PriceBitSet` — bitmap for O(1) best-price scan and next-level traversal
- `FastHashmap` — Robin Hood open-addressing hash map for book lookup by `BookKey` and order reverse-index by `OrderId`

### Lock-free SPSC queue

`SpscQueue<T, N>` (`include/spsc.h`) is a pre-allocated lock-free ring buffer (power-of-2 capacity). Each `WorkerBucket` has one. Producers (submit threads) push; the bucket's worker thread pops. Only single-producer, single-consumer is safe.

### Zero-heap data types

- `FixedStr<N>` (`include/fixed_str.h`) — stack-allocated string for IDs (OrderId, SecurityId, etc.). Avoids heap fragmentation on hot paths.
- `FastHashmap<K, V>` (`include/fast_hashmap.h`) — Robin Hood hash map with backward-shift deletion. Used for order book index and order reverse-lookup.
- Command variants (`CmdOrder`, `CmdCancel`, etc.) carry parsed structs through the SPSC queue, avoiding JSON parse/re-parse costs in the worker thread.

### Admin interface

`AdminServer` (`include/admin_server.h`, `src/admin_server.cpp`) is a TCP server (port 32000) using epoll. Python backend (`admin/server.py`, FastAPI + WebSocket) bridges to a Streamlit frontend (`admin/app.py`). Communication uses JSON Lines protocol.

### TradeLogger

`TradeLogger` (`include/trade_logger.h`, `src/trade_logger.cpp`) writes all trading events (order new/confirm/reject/execution/cancel, market data) as JSONL with millisecond timestamps via an async background writer thread.

### Outputs / executables

- `bin/` — all compiled binaries (set via `EXECUTABLE_OUTPUT_PATH`)
- `lib/` — `libtrade_engine.a`
- Key benchmarks: `benchmark` (throughput), `bench_matching` (matching-only), `bench_concurrent` (multi-thread contention), `bench_multicore` (multi-bucket scaling), `latency_e2e` (end-to-end latency with TSC measurements)
- Tests live in `tests/`, benchmarks in `benchmarks/`, example programs in `examples/`
