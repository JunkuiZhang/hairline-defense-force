/**
 * @file latency_multicore.cpp
 * @brief 多 Bucket 并行扩展性压测 (单生产者低延迟改造版)
 *
 * 比较不同 bucket 数量下、单个生产者持续提交时的吞吐量变化。
 * 支持对比「绑核模式 (Pinned)」与「不绑核模式 (Unpinned)」的性能差异。
 */

#include "trade_system.h"
#include "utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ── 全局变量：TSC 频率 (GHz) ──────────────────────────────────
static double g_tsc_ghz = 1.0;

// ── 配置 ─────────────────────────────────────────────────────
struct Config {
    int totalOrders = 200000;
    int numSecurities = 50;
    int numShareholders = 500;
    int warmupOrders = 2000;
    std::string mode = "all"; // 可选: "all", "pinned", "unpinned"
};

static Config parseArgs(int argc, char *argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--orders" && i + 1 < argc)
            cfg.totalOrders = std::atoi(argv[++i]);
        else if (a == "--mode" && i + 1 < argc)
            cfg.mode = argv[++i];
        else if (a == "--securities" && i + 1 < argc)
            cfg.numSecurities = std::atoi(argv[++i]);
        else if (a == "--shareholders" && i + 1 < argc)
            cfg.numShareholders = std::atoi(argv[++i]);
        else if (a == "--warmup" && i + 1 < argc)
            cfg.warmupOrders = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::cout << "用法: bench_multicore [选项]\n"
                      << "  --orders N          总订单量 (默认 200000)\n"
                      << "  --mode M            测试模式: all, pinned, "
                         "unpinned (默认 all)\n"
                      << "  --securities M      证券种类数 (默认 50)\n"
                      << "  --shareholders K    股东数 (默认 500)\n"
                      << "  --warmup W          热身订单数 (默认 2000)\n";
            std::exit(0);
        }
    }
    return cfg;
}

// ── 订单生成（直接构造 struct，零 JSON）──────────────────────
static std::vector<hdf::Order> generateOrders(const Config &cfg, int count,
                                              int idOffset, int seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> secDist(0, cfg.numSecurities - 1);
    std::uniform_int_distribution<int> shDist(0, cfg.numShareholders - 1);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_real_distribution<double> priceDist(5.0, 50.0);
    std::uniform_int_distribution<int> qtyDist(1, 100);

    std::vector<std::string> securities(cfg.numSecurities);
    for (int i = 0; i < cfg.numSecurities; ++i) {
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(6) << (600000 + i);
        securities[i] = oss.str();
    }

    std::vector<std::string> shareholders(cfg.numShareholders);
    for (int i = 0; i < cfg.numShareholders; ++i) {
        std::ostringstream oss;
        oss << "SH" << std::setfill('0') << std::setw(4) << i;
        shareholders[i] = oss.str();
    }

    std::vector<hdf::Order> orders;
    orders.reserve(count);
    for (int i = 0; i < count; ++i) {
        hdf::Order o;
        o.clOrderId = "M" + std::to_string(idOffset + i);
        o.market = hdf::Market::XSHG;
        o.securityId = securities[secDist(rng)];
        o.side = sideDist(rng) == 0 ? hdf::Side::BUY : hdf::Side::SELL;
        o.price = std::round(priceDist(rng) * 100.0) / 100.0;
        o.qty = qtyDist(rng) * 100;
        o.shareholderId = shareholders[shDist(rng)];
        orders.push_back(o);
    }
    return orders;
}

// ── 延时统计 ─────────────────────────────────────────────────
struct LatStats {
    double mean_cycles = 0, p50_cycles = 0, p95_cycles = 0, p99_cycles = 0,
           max_cycles = 0;
    size_t count = 0;
};

static LatStats calcStats(std::vector<uint64_t> &lat_cycles) {
    LatStats s{};
    s.count = lat_cycles.size();
    if (s.count == 0)
        return s;
    std::sort(lat_cycles.begin(), lat_cycles.end());
    s.mean_cycles =
        std::accumulate(lat_cycles.begin(), lat_cycles.end(), 0.0) / s.count;
    auto pct = [&](double p) {
        return (double)lat_cycles[std::min(size_t(p / 100.0 * (s.count - 1)),
                                           s.count - 1)];
    };
    s.p50_cycles = pct(50);
    s.p95_cycles = pct(95);
    s.p99_cycles = pct(99);
    s.max_cycles = (double)lat_cycles.back();
    return s;
}

// ── 单轮结果 ─────────────────────────────────────────────────
struct RoundResult {
    int numBuckets;
    bool isPinned;
    double elapsed_s;
    double throughput;
    LatStats e2eLat;
    LatStats submitLat;
    size_t execReports;
    size_t rejectReports;
};

// ── 单轮测试：N buckets + 1 producer ───────────────────────
static RoundResult runRound(const Config &cfg, int numBuckets, bool isPinned) {
    // 构造 worker 核心列表
    std::vector<int> worker_cores;
    if (isPinned) {
        // 按照要求，绑核在 4, 6, 8, 10
        int target_cores[] = {4, 6, 8, 10};
        for (int i = 0; i < numBuckets; ++i) {
            worker_cores.push_back(target_cores[i % 4]);
        }
    }

    hdf::TradeSystem system(worker_cores);

    std::atomic<size_t> matchedCount{0};
    std::atomic<size_t> rejectedCount{0};
    int actualTotal = cfg.totalOrders;

    struct OrderTiming {
        uint64_t submit_cycles = 0;
        uint64_t first_response_cycles = 0;
    };
    std::vector<OrderTiming> timings(actualTotal);

    auto parseIdx = [&](const hdf::OrderId &id) -> int {
        if (id.empty() || id.data[0] != 'M')
            return -1;
        int x = std::atoi(id.c_str() + 1);
        int idx = x - cfg.warmupOrders;
        return (idx >= 0 && idx < actualTotal) ? idx : -1;
    };

    system.setSendToClient([&](const hdf::ClientReport &report) {
        hdf::OrderId orderId;
        std::visit(
            [&](const auto &r) {
                using T = std::decay_t<decltype(r)>;
                if constexpr (std::is_same_v<T, hdf::OrderResponse>) {
                    if (!r.execId.empty())
                        matchedCount.fetch_add(1, std::memory_order_relaxed);
                    else if (r.rejectCode != 0)
                        rejectedCount.fetch_add(1, std::memory_order_relaxed);
                    orderId = r.clOrderId;
                } else {
                    orderId = r.clOrderId;
                }
            },
            report);
        int idx = parseIdx(orderId);
        if (idx >= 0 && timings[idx].first_response_cycles == 0) {
            timings[idx].first_response_cycles = hdf::rdtscp_lfence();
        }
    });

    auto warmupOrders = generateOrders(cfg, cfg.warmupOrders, 0, 9999);
    for (auto &order : warmupOrders) {
        system.handleOrder(nlohmann::json(order));
    }
    matchedCount = 0;
    rejectedCount = 0;

    system.startEventLoop();

    auto producerOrders =
        generateOrders(cfg, cfg.totalOrders, cfg.warmupOrders, 42);

    std::vector<uint64_t> submitLats_cycles;
    submitLats_cycles.reserve(cfg.totalOrders);

    uint64_t wallStart = hdf::rdtsc_lfence();

    std::thread prodThread([&]() {
        // 生产者始终固定在 Core 3，避免与主线程和 Worker 抢占
        hdf::pin_to_core(3);

        for (int i = 0; i < cfg.totalOrders; ++i) {
            uint64_t t0 = hdf::rdtsc_lfence();
            timings[i].submit_cycles = t0;

            system.submitOrder(producerOrders[i]);

            uint64_t t1 = hdf::rdtscp_lfence();
            submitLats_cycles.push_back(t1 - t0);
        }
    });

    prodThread.join();

    system.stopEventLoop();
    uint64_t wallEnd = hdf::rdtscp_lfence();

    double elapsed_s = (wallEnd - wallStart) / g_tsc_ghz / 1e9;

    std::vector<uint64_t> allE2E_cycles;
    allE2E_cycles.reserve(actualTotal);
    for (int i = 0; i < actualTotal; ++i) {
        if (timings[i].submit_cycles > 0 &&
            timings[i].first_response_cycles > timings[i].submit_cycles) {
            allE2E_cycles.push_back(timings[i].first_response_cycles -
                                    timings[i].submit_cycles);
        }
    }

    RoundResult r;
    r.numBuckets = numBuckets;
    r.isPinned = isPinned;
    r.elapsed_s = elapsed_s;
    r.throughput = actualTotal / elapsed_s;
    r.e2eLat = calcStats(allE2E_cycles);
    r.submitLat = calcStats(submitLats_cycles);
    r.execReports = matchedCount.load();
    r.rejectReports = rejectedCount.load();
    return r;
}

// ── 输出 ─────────────────────────────────────────────────────
static void printRound(const RoundResult &r) {
    auto to_ns = [](double cycles) { return cycles / g_tsc_ghz; };
    std::string modeStr = r.isPinned ? "绑核 (Pinned)" : "不绑核 (Unpinned)";

    std::cout << "\n──────────────────────────────────────────────────\n";
    std::cout << "  模式: " << modeStr << "   Buckets: " << r.numBuckets
              << "   Producers: 1\n";
    std::cout << "  订单数: " << r.e2eLat.count << "   耗时: " << std::fixed
              << std::setprecision(3) << r.elapsed_s << "s"
              << "   成交: " << r.execReports << "   拒绝: " << r.rejectReports
              << "\n";
    std::cout << "  吞吐量: " << std::fixed << std::setprecision(0)
              << r.throughput << " orders/s\n\n";

    std::cout << std::fixed << std::setprecision(1);

    std::cout << "  E2E 延时:\n"
              << "    p50:  " << std::setw(10) << r.e2eLat.p50_cycles
              << " cycles (" << std::setw(8) << to_ns(r.e2eLat.p50_cycles)
              << " ns)\n"
              << "    p99:  " << std::setw(10) << r.e2eLat.p99_cycles
              << " cycles (" << std::setw(8) << to_ns(r.e2eLat.p99_cycles)
              << " ns)\n";

    std::cout << "  入队延时 (Enq):\n"
              << "    p50:  " << std::setw(10) << r.submitLat.p50_cycles
              << " cycles (" << std::setw(8) << to_ns(r.submitLat.p50_cycles)
              << " ns)\n"
              << "    p99:  " << std::setw(10) << r.submitLat.p99_cycles
              << " cycles (" << std::setw(8) << to_ns(r.submitLat.p99_cycles)
              << " ns)\n";
}

static void printSummary(const std::vector<RoundResult> &results,
                         const std::string &title) {
    if (results.empty())
        return;
    auto to_ns = [](double cycles) { return cycles / g_tsc_ghz; };

    std::cout << "\n\n╔════════════════════════════════════════════════════════"
                 "═════════════════════════╗\n";
    std::cout << "║  " << std::left << std::setw(77) << title << "║\n";
    std::cout << "╠═════════╦═══════════╦══════════════╦══════════════╦════════"
                 "══════╦══════════════╣\n";
    std::cout << "║ Buckets ║ Throughput║ E2E-p50 (ns) ║ E2E-p99 (ns) ║ "
                 "Enq-p50 (ns) ║   加速比     ║\n";
    std::cout << "╠═════════╬═══════════╬══════════════╬══════════════╬════════"
                 "══════╬══════════════╣\n";

    double baseline = results[0].throughput;
    for (auto &r : results) {
        double speedup = r.throughput / baseline;
        std::cout << std::fixed;
        std::cout << "║ " << std::setw(7) << r.numBuckets << " ║ "
                  << std::setw(9) << std::setprecision(0) << r.throughput
                  << " ║ " << std::setw(12) << std::setprecision(1)
                  << to_ns(r.e2eLat.p50_cycles) << " ║ " << std::setw(12)
                  << std::setprecision(1) << to_ns(r.e2eLat.p99_cycles) << " ║ "
                  << std::setw(12) << std::setprecision(1)
                  << to_ns(r.submitLat.p50_cycles) << " ║ " << std::setw(11)
                  << std::setprecision(2) << speedup << "x ║\n";
    }
    std::cout << "╚═════════╩═══════════╩══════════════╩══════════════╩════════"
                 "══════╩══════════════╝\n";
}

// ── main ─────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    // ⚠️ 注意：这里故意【不】对主线程进行绑核。
    // 因为如果主线程绑定在某个核上，在“不绑核
    // (Unpinned)”模式下启动的消费者子线程
    // 会自动继承主线程的亲和性，导致所有消费者全部挤在一个核上，破坏测试公平性。
    g_tsc_ghz = hdf::calibrate_tsc_ghz();

    Config cfg = parseArgs(argc, argv);

    unsigned int hwThreads = std::thread::hardware_concurrency();
    std::cout << "=== 多 Bucket 并行扩展性 Benchmark (单生产者对比版) ===\n"
              << "  TSC 频率: " << g_tsc_ghz << " GHz\n"
              << "  总订单: " << cfg.totalOrders
              << "  证券数: " << cfg.numSecurities << "\n"
              << "  硬件线程数: " << hwThreads << "\n";

    std::vector<RoundResult> unpinnedResults;
    std::vector<RoundResult> pinnedResults;

    // --- 不绑核模式测试 ---
    if (cfg.mode == "all" || cfg.mode == "unpinned") {
        std::vector<int> unpinnedCounts = {1, 2, 4, 8, 16};
        std::cout << "\n>>> [阶段 1] 开始不绑核 (Unpinned) 模式测试...\n";
        for (int nBuckets : unpinnedCounts) {
            auto r = runRound(cfg, nBuckets, false);
            unpinnedResults.push_back(r);
            printRound(r);
        }
    }

    // --- 绑核模式测试 ---
    if (cfg.mode == "all" || cfg.mode == "pinned") {
        std::vector<int> pinnedCounts = {1, 2, 4};
        std::cout << "\n>>> [阶段 2] 开始绑核 (Pinned) 模式测试 (Cores: 4, 6, "
                     "8, 10)...\n";
        for (int nBuckets : pinnedCounts) {
            auto r = runRound(cfg, nBuckets, true);
            pinnedResults.push_back(r);
            printRound(r);
        }
    }

    // 汇总输出对比表格
    if (!unpinnedResults.empty()) {
        printSummary(unpinnedResults,
                     "多 Bucket 并行扩展性测试 - 不绑核 (Unpinned)");
    }
    if (!pinnedResults.empty()) {
        printSummary(pinnedResults, "多 Bucket 并行扩展性测试 - 绑核 (Pinned)");
    }

    return 0;
}