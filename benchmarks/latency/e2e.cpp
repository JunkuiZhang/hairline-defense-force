/**
 * @file e2e.cpp
 * @brief 端到端订单延迟 Benchmark
 *
 * 预挂买单入簿后（买方与卖方使用不同股东号，避免对敲拒绝），
 * 以受控速率连续提交卖单并记录 start TSC。
 * 回调中记录 end TSC，通过 clOrderId 反查索引匹配。
 * 通过 pace 控制提交速率，逼近纯处理延迟。
 *
 * 延迟组成: SPSC push → worker pop → dispatch → matching → callback
 * 其中 dispatch 部分 (含风控+撮合+回报) 约占 ~15k cycles (5us)
 *
 * 注意：Worker 线程使用纯忙等（不 yield），测试时需固定 CPU 核心避免干扰。
 */
#include "trade_system.h"
#include "utils.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>
#include <x86intrin.h>

using namespace hdf;

constexpr int WARMUP = 2000;
constexpr int ITERATIONS = 100000;
constexpr int TOTAL = WARMUP + ITERATIONS;

int main() {
    double tsc_ghz = hdf::calibrate_tsc_ghz();
    std::cout << std::unitbuf;
    std::cout << "=== E2E Ping-Pong Latency Benchmark ===\n";
    std::cout << "TSC Frequency: " << std::fixed << std::setprecision(4)
              << tsc_ghz << " GHz\n";
    std::cout << "初始化系统 (1 Bucket)...\n";

    hdf::pin_to_core(2);

    TradeSystem ts(std::vector<int>{3});  // pin worker to core 3, main on core 2

    std::vector<uint64_t> end_tscs(ITERATIONS, 0);
    std::atomic<size_t> done_count{0};

    ts.setSendToClient([&](const ClientReport &report) {
        std::visit(
            [&](const auto &r) {
                using T = std::decay_t<decltype(r)>;
                if constexpr (std::is_same_v<T, OrderResponse>) {
                    if ((!r.execId.empty() || r.rejectCode != 0) &&
                        r.side == Side::SELL) {
                        int idx = std::atoi(r.clOrderId.c_str() + 1);
                        if (idx >= WARMUP && idx < TOTAL) {
                            end_tscs[idx - WARMUP] = hdf::rdtscp_lfence();
                            done_count.fetch_add(1,
                                                 std::memory_order_release);
                        }
                    }
                }
            },
            report);
    });

    // ── 预创建订单 ──
    std::vector<Order> buys(TOTAL);
    std::vector<Order> sells(TOTAL);
    for (int i = 0; i < TOTAL; ++i) {
        auto id = std::to_string(i);
        buys[i].clOrderId = ("B" + id).c_str();
        buys[i].market = Market::XSHG;
        buys[i].securityId = "600000";
        buys[i].side = Side::BUY;
        buys[i].price = 10.0 + i * 0.01;
        buys[i].qty = 100;
        buys[i].shareholderId = "BUYER";

        sells[i].clOrderId = ("S" + id).c_str();
        sells[i].market = Market::XSHG;
        sells[i].securityId = "600000";
        sells[i].side = Side::SELL;
        sells[i].price = 10.0 + i * 0.01;
        sells[i].qty = 100;
        sells[i].shareholderId = "SELLER";
    }

    ts.startEventLoop();

    // ── 预挂买单 ──
    std::cout << "预挂 " << TOTAL << " 笔买单入簿...\n";
    for (int i = 0; i < TOTAL; ++i) {
        ts.submitOrder(buys[i]);
    }
    while (ts.queueDepth() > 0) {
        _mm_pause();
    }

    // ── 预热 + 测量连续提交 ──
    // 倒序提交卖单（高价 → 低价），让每笔卖单撮合同价位的买单，
    // 而非逐笔消耗最优买单导致后期卖单无法成交。
    std::vector<uint64_t> start_tscs(ITERATIONS);
    std::cout << "预热 " << WARMUP << " + 测量 " << ITERATIONS << " 笔...\n";

    // Worker 纯忙等无需唤醒；前 WARMUP 笔自然预热匹配代码路径
    constexpr uint64_t PACE_CYCLES = 20000;
    for (int i = TOTAL - 1; i >= 0; --i) {
        uint64_t t0 = hdf::rdtsc_lfence();
        ts.submitOrder(sells[i]);
        if (i >= WARMUP) {
            start_tscs[i - WARMUP] = t0;
        }
        while (hdf::rdtsc_lfence() - t0 < PACE_CYCLES) {
            _mm_pause();
        }
    }

    while (done_count.load(std::memory_order_acquire) < (size_t)ITERATIONS) {
        _mm_pause();
    }

    ts.stopEventLoop();

    // ── 计算延迟 ──
    std::vector<uint64_t> latencies;
    latencies.reserve(ITERATIONS);
    for (int i = 0; i < ITERATIONS; ++i) {
        if (end_tscs[i] > start_tscs[i]) {
            latencies.push_back(end_tscs[i] - start_tscs[i]);
        }
    }

    // ── 统计 ──
    std::sort(latencies.begin(), latencies.end());

    auto to_ns = [tsc_ghz](uint64_t cycles) { return cycles / tsc_ghz; };

    double mean =
        std::accumulate(latencies.begin(), latencies.end(), 0.0) /
        latencies.size();

    auto pct = [&](double p) {
        return latencies[size_t(p / 100.0 * (latencies.size() - 1))];
    };

    std::cout << "\n=== 测试结果 (N=" << latencies.size() << ") ===\n";
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "  Mean: " << std::setw(8) << mean << " cycles ("
              << std::setw(8) << to_ns((uint64_t)mean) << " ns)\n";
    std::cout << "  p50 : " << std::setw(8) << pct(50) << " cycles ("
              << std::setw(8) << to_ns(pct(50)) << " ns)\n";
    std::cout << "  p90 : " << std::setw(8) << pct(90) << " cycles ("
              << std::setw(8) << to_ns(pct(90)) << " ns)\n";
    std::cout << "  p95 : " << std::setw(8) << pct(95) << " cycles ("
              << std::setw(8) << to_ns(pct(95)) << " ns)\n";
    std::cout << "  p99 : " << std::setw(8) << pct(99) << " cycles ("
              << std::setw(8) << to_ns(pct(99)) << " ns)\n";
    std::cout << "  Max : " << std::setw(8) << latencies.back() << " cycles ("
              << std::setw(8) << to_ns(latencies.back()) << " ns)\n";

    return 0;
}
