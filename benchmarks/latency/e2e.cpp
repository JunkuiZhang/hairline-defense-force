#include "trade_system.h"
#include "utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>
#include <x86intrin.h>

using namespace hdf;

// 测算 TSC 频率
static double get_tsc_ghz() {
    uint64_t start = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    uint64_t end = __rdtsc();
    return (end - start) / 100000000.0;
}

int main() {
    std::cout << "=== E2E Ping-Pong Latency Benchmark ===\n";
    std::cout << "初始化系统 (1 Bucket)...\n";
    
    double tsc_ghz = get_tsc_ghz();
    std::cout << "TSC Frequency: " << std::fixed << std::setprecision(4) << tsc_ghz << " GHz\n";

    TradeSystem ts(1);
    
    std::atomic<uint64_t> ack_tsc{0};
    
    ts.setSendToClient([&](const ClientReport &report) {
        std::visit([&](const auto &r) {
            using T = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<T, OrderResponse>) {
                if (!r.execId.empty() || r.rejectCode != 0) {
                    ack_tsc.store(__rdtsc(), std::memory_order_relaxed);
                }
            }
        }, report);
    });

    ts.startEventLoop();

    const int ITERATIONS = 100000;
    std::vector<uint64_t> latencies;
    latencies.reserve(ITERATIONS);

    std::cout << "正在测量 (无排队干扰的最纯粹端到端延迟)...\n";

    for (int i = 0; i < ITERATIONS; ++i) {
        // 先挂一个买单
        Order buy;
        buy.clOrderId = "B" + std::to_string(i);
        buy.market = Market::XSHG;
        buy.securityId = "600000";
        buy.side = Side::BUY;
        buy.price = 10.0;
        buy.qty = 100;
        buy.shareholderId = "SH001";
        
        ts.submitOrder(buy);
        
        // 给一点点时间让买单进入引擎并挂在账本上
        // 这就保证了队列一直是空的，完全没有排队积压
        for(int volatile j=0; j<5000; ++j) { _mm_pause(); }

        // 再发一个能立即成交的卖单
        Order sell;
        sell.clOrderId = "S" + std::to_string(i);
        sell.market = Market::XSHG;
        sell.securityId = "600000";
        sell.side = Side::SELL;
        sell.price = 10.0;
        sell.qty = 100;
        sell.shareholderId = "SH001";

        ack_tsc.store(0, std::memory_order_relaxed);
        
        uint64_t start = __rdtsc();
        ts.submitOrder(sell);
        
        // 自旋等待回调触发
        while (ack_tsc.load(std::memory_order_relaxed) == 0) {
            _mm_pause();
        }
        uint64_t end = ack_tsc.load(std::memory_order_relaxed);
        
        latencies.push_back(end - start);
    }

    ts.stopEventLoop();

    // 统计
    std::sort(latencies.begin(), latencies.end());
    
    auto to_ns = [tsc_ghz](uint64_t cycles) {
        return cycles / tsc_ghz;
    };

    double mean = 0;
    for (auto c : latencies) mean += c;
    mean /= latencies.size();

    std::cout << "\n=== 测试结果 (N=" << ITERATIONS << ") ===\n";
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "  Mean: " << std::setw(8) << mean << " cycles (" << std::setw(8) << to_ns(mean) << " ns)\n";
    std::cout << "  p50 : " << std::setw(8) << latencies[ITERATIONS * 0.5] << " cycles (" << std::setw(8) << to_ns(latencies[ITERATIONS * 0.5]) << " ns)\n";
    std::cout << "  p90 : " << std::setw(8) << latencies[ITERATIONS * 0.90] << " cycles (" << std::setw(8) << to_ns(latencies[ITERATIONS * 0.90]) << " ns)\n";
    std::cout << "  p95 : " << std::setw(8) << latencies[ITERATIONS * 0.95] << " cycles (" << std::setw(8) << to_ns(latencies[ITERATIONS * 0.95]) << " ns)\n";
    std::cout << "  p99 : " << std::setw(8) << latencies[ITERATIONS * 0.99] << " cycles (" << std::setw(8) << to_ns(latencies[ITERATIONS * 0.99]) << " ns)\n";
    std::cout << "  Max : " << std::setw(8) << latencies.back() << " cycles (" << std::setw(8) << to_ns(latencies.back()) << " ns)\n";

    return 0;
}
