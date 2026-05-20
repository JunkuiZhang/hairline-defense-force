/**
 * @file latency_matching.cpp
 * @brief 撮合引擎专项 Benchmark (Low-Latency 改造版)
 *
 * 绕过 JSON 解析、风控、日志，直接用 Order 结构体驱动 MatchingEngine，
 * 隔离测量 addOrder / match 各自的延时和吞吐。
 * 使用 RDTSC 指令记录周期数 (Cycles) 并转换为纳秒 (ns) 输出。
 */

#include "matching_engine.h"
#include "utils.h"
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

// ── 全局变量：TSC 频率 (GHz) ──────────────────────────────────
static double g_tsc_ghz = 1.0;

// ── 配置 ────────────────────────────────────────────────────
struct Config {
    int numOrders = 100000; // 测量的订单数
    int numSecurities = 10; // 证券种类
    int bookDepth = 5000;   // 每只证券预挂单深度（买+卖各一半）
    int warmup = 1000;      // 热身订单数
};

// ── 订单工厂 ────────────────────────────────────────────────
static std::string makeSecId(int i) {
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(6) << (600000 + i);
    return oss.str();
}

static hdf::Order makeOrder(int id, const std::string &secId, hdf::Side side,
                            double price, uint32_t qty) {
    hdf::Order o;
    o.clOrderId = "M" + std::to_string(id);
    o.market = hdf::Market::XSHG;
    o.securityId = secId;
    o.side = side;
    o.price = price;
    o.qty = qty;
    o.shareholderId = "SH0001";
    return o;
}

// ── 延时统计 ────────────────────────────────────────────────
struct LatStats {
    double mean_cycles, p50_cycles, p95_cycles, p99_cycles, max_cycles;
    double throughput;
    size_t count;
};

// 现在直接接收纯净的 TSC 周期数进行统计
static LatStats calcStats(std::vector<uint64_t> &lat_cycles, double elapsed_s) {
    LatStats s{};
    s.count = lat_cycles.size();
    if (s.count == 0)
        return s;
    std::sort(lat_cycles.begin(), lat_cycles.end());

    s.mean_cycles =
        std::accumulate(lat_cycles.begin(), lat_cycles.end(), 0.0) / s.count;
    auto pct = [&](double p) {
        return (double)lat_cycles[size_t(p / 100.0 * (s.count - 1))];
    };
    s.p50_cycles = pct(50);
    s.p95_cycles = pct(95);
    s.p99_cycles = pct(99);
    s.max_cycles = (double)lat_cycles.back();

    s.throughput = s.count / elapsed_s;
    return s;
}

static void printStats(const LatStats &s, const std::string &label) {
    // 换算公式: ns = cycles / GHz
    auto to_ns = [](double cycles) { return cycles / g_tsc_ghz; };

    std::cout << std::fixed << std::setprecision(1); // 保留一位小数
    std::cout << "  [" << label << "]  N=" << s.count
              << "  tput=" << (uint64_t)s.throughput << " op/s\n";

    // 对齐打印 Cycles 和 ns
    std::cout << "    mean: " << std::setw(8) << s.mean_cycles << " cycles  ("
              << std::setw(8) << to_ns(s.mean_cycles) << " ns)\n"
              << "    p50:  " << std::setw(8) << s.p50_cycles << " cycles  ("
              << std::setw(8) << to_ns(s.p50_cycles) << " ns)\n"
              << "    p95:  " << std::setw(8) << s.p95_cycles << " cycles  ("
              << std::setw(8) << to_ns(s.p95_cycles) << " ns)\n"
              << "    p99:  " << std::setw(8) << s.p99_cycles << " cycles  ("
              << std::setw(8) << to_ns(s.p99_cycles) << " ns)\n"
              << "    max:  " << std::setw(8) << s.max_cycles << " cycles  ("
              << std::setw(8) << to_ns(s.max_cycles) << " ns)\n";
}

// ── Bench 1: addOrder ───────────────────────────────────────
static void benchAddOrder(const Config &cfg) {
    std::cout << "\n=== Bench: addOrder ===\n";
    hdf::MatchingEngine engine;
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> secDist(0, cfg.numSecurities - 1);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_real_distribution<double> priceDist(5.0, 50.0);
    std::uniform_int_distribution<int> qtyDist(1, 100);

    std::vector<std::string> secIds(cfg.numSecurities);
    for (int i = 0; i < cfg.numSecurities; ++i)
        secIds[i] = makeSecId(i);

    // 预生成订单
    std::vector<hdf::Order> orders;
    orders.reserve(cfg.warmup + cfg.numOrders);
    for (int i = 0; i < cfg.warmup + cfg.numOrders; ++i) {
        auto side = sideDist(rng) == 0 ? hdf::Side::BUY : hdf::Side::SELL;
        uint32_t qty = qtyDist(rng) * 100;
        double price = std::round(priceDist(rng) * 100.0) / 100.0;
        orders.push_back(makeOrder(i, secIds[secDist(rng)], side, price, qty));
    }

    for (int i = 0; i < cfg.warmup; ++i)
        engine.addOrder(orders[i]);

    // 使用 uint64_t 记录原生的 CPU 周期差
    std::vector<uint64_t> lat_cycles;
    lat_cycles.reserve(cfg.numOrders);

    uint64_t wall0 = hdf::rdtsc_lfence();
    for (int i = cfg.warmup; i < cfg.warmup + cfg.numOrders; ++i) {
        uint64_t t0 = hdf::rdtsc_lfence();
        engine.addOrder(orders[i]);
        uint64_t t1 = hdf::rdtscp_lfence();
        lat_cycles.push_back(t1 - t0); // 只存周期数，极低开销
    }
    uint64_t wall1 = hdf::rdtscp_lfence();

    double elapsed_s = (wall1 - wall0) / g_tsc_ghz / 1e9;
    auto s = calcStats(lat_cycles, elapsed_s);
    printStats(s, "addOrder");
}

// ── Bench 2: match（有成交） ────────────────────────────────
static void benchMatchHit(const Config &cfg) {
    std::cout << "\n=== Bench: match (有成交) ===\n";

    hdf::MatchingEngine engine;
    std::mt19937 rng(5678);
    std::uniform_int_distribution<int> secDist(0, cfg.numSecurities - 1);
    std::uniform_real_distribution<double> priceDist(10.0, 40.0);
    std::uniform_int_distribution<int> qtyDist(1, 50);

    std::vector<std::string> secIds(cfg.numSecurities);
    for (int i = 0; i < cfg.numSecurities; ++i)
        secIds[i] = makeSecId(i);

    int id = 0;
    int halfDepth = cfg.bookDepth / 2;
    for (int sec = 0; sec < cfg.numSecurities; ++sec) {
        for (int d = 0; d < halfDepth; ++d) {
            double buyPrice = 10.0 + d * 0.01;
            engine.addOrder(makeOrder(id++, secIds[sec], hdf::Side::BUY,
                                      buyPrice, qtyDist(rng) * 100));
        }
        for (int d = 0; d < halfDepth; ++d) {
            double sellPrice = 10.0 + d * 0.01;
            engine.addOrder(makeOrder(id++, secIds[sec], hdf::Side::SELL,
                                      sellPrice, qtyDist(rng) * 100));
        }
    }
    std::cout << "  订单簿已填充 " << id << " 笔\n";

    std::vector<hdf::Order> testOrders;
    testOrders.reserve(cfg.warmup + cfg.numOrders);
    for (int i = 0; i < cfg.warmup + cfg.numOrders; ++i) {
        int sec = secDist(rng);
        bool isBuy = (i % 2 == 0);
        double price = isBuy ? 50.0 : 5.0;
        uint32_t qty = 100;
        auto side = isBuy ? hdf::Side::BUY : hdf::Side::SELL;
        testOrders.push_back(makeOrder(id++, secIds[sec], side, price, qty));
    }

    for (int i = 0; i < cfg.warmup; ++i)
        engine.match(testOrders[i]);

    std::vector<uint64_t> lat_cycles;
    lat_cycles.reserve(cfg.numOrders);
    size_t totalExecs = 0;

    uint64_t wall0 = hdf::rdtsc_lfence();
    for (int i = cfg.warmup; i < cfg.warmup + cfg.numOrders; ++i) {
        uint64_t t0 = hdf::rdtsc_lfence();
        auto result = engine.match(testOrders[i]);
        uint64_t t1 = hdf::rdtscp_lfence();

        lat_cycles.push_back(t1 - t0);
        totalExecs += result.executions.size();

        if (result.remainingQty > 0) {
            hdf::Order rem = testOrders[i];
            rem.qty = result.remainingQty;
            engine.addOrder(rem);
        }
    }
    uint64_t wall1 = hdf::rdtscp_lfence();

    double elapsed_s = (wall1 - wall0) / g_tsc_ghz / 1e9;
    auto s = calcStats(lat_cycles, elapsed_s);
    printStats(s, "match(hit)");
    std::cout << "    成交笔数: " << totalExecs << "\n";
}

// ── Bench 3: match（无成交 / miss） ────────────────────────
static void benchMatchMiss(const Config &cfg) {
    std::cout << "\n=== Bench: match (无成交) ===\n";

    hdf::MatchingEngine engine;
    std::mt19937 rng(9999);
    std::uniform_int_distribution<int> secDist(0, cfg.numSecurities - 1);

    std::vector<std::string> secIds(cfg.numSecurities);
    for (int i = 0; i < cfg.numSecurities; ++i)
        secIds[i] = makeSecId(i);

    int id = 0;
    for (int sec = 0; sec < cfg.numSecurities; ++sec) {
        for (int d = 0; d < cfg.bookDepth; ++d) {
            double price = 10.0 + d * 0.01;
            engine.addOrder(
                makeOrder(id++, secIds[sec], hdf::Side::BUY, price, 100));
        }
    }
    std::cout << "  订单簿已填充 " << id << " 笔 (仅买单)\n";

    std::vector<hdf::Order> testOrders;
    testOrders.reserve(cfg.warmup + cfg.numOrders);
    for (int i = 0; i < cfg.warmup + cfg.numOrders; ++i) {
        int sec = secDist(rng);
        testOrders.push_back(
            makeOrder(id++, secIds[sec], hdf::Side::SELL, 999.0, 100));
    }

    for (int i = 0; i < cfg.warmup; ++i)
        engine.match(testOrders[i]);

    std::vector<uint64_t> lat_cycles;
    lat_cycles.reserve(cfg.numOrders);

    uint64_t wall0 = hdf::rdtsc_lfence();
    for (int i = cfg.warmup; i < cfg.warmup + cfg.numOrders; ++i) {
        uint64_t t0 = hdf::rdtsc_lfence();
        auto result = engine.match(testOrders[i]);
        uint64_t t1 = hdf::rdtscp_lfence();
        lat_cycles.push_back(t1 - t0);
    }
    uint64_t wall1 = hdf::rdtscp_lfence();

    double elapsed_s = (wall1 - wall0) / g_tsc_ghz / 1e9;
    auto s = calcStats(lat_cycles, elapsed_s);
    printStats(s, "match(miss)");
}

// ── Bench 4: cancelOrder ────────────────────────────────────
static void benchCancel(const Config &cfg) {
    std::cout << "\n=== Bench: cancelOrder ===\n";

    hdf::MatchingEngine engine;
    std::mt19937 rng(7777);
    std::uniform_int_distribution<int> secDist(0, cfg.numSecurities - 1);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_real_distribution<double> priceDist(5.0, 50.0);

    std::vector<std::string> secIds(cfg.numSecurities);
    for (int i = 0; i < cfg.numSecurities; ++i)
        secIds[i] = makeSecId(i);

    int total = cfg.warmup + cfg.numOrders;
    std::vector<hdf::OrderId> orderIds;
    orderIds.reserve(total);
    for (int i = 0; i < total; ++i) {
        auto side = sideDist(rng) == 0 ? hdf::Side::BUY : hdf::Side::SELL;
        double price = std::round(priceDist(rng) * 100.0) / 100.0;
        auto o = makeOrder(i, secIds[secDist(rng)], side, price, 100);
        engine.addOrder(o);
        orderIds.push_back(o.clOrderId);
    }

    std::shuffle(orderIds.begin(), orderIds.end(), rng);

    for (int i = 0; i < cfg.warmup; ++i)
        engine.cancelOrder(orderIds[i]);

    std::vector<uint64_t> lat_cycles;
    lat_cycles.reserve(cfg.numOrders);

    uint64_t wall0 = hdf::rdtsc_lfence();
    for (int i = cfg.warmup; i < total; ++i) {
        uint64_t t0 = hdf::rdtsc_lfence();
        engine.cancelOrder(orderIds[i]);
        uint64_t t1 = hdf::rdtscp_lfence();
        lat_cycles.push_back(t1 - t0);
    }
    uint64_t wall1 = hdf::rdtscp_lfence();

    double elapsed_s = (wall1 - wall0) / g_tsc_ghz / 1e9;
    auto s = calcStats(lat_cycles, elapsed_s);
    printStats(s, "cancelOrder");
}

// ── main ────────────────────────────────────────────────────
int main() {
    int test_core_id = 2;
    hdf::pin_to_core(test_core_id);
    g_tsc_ghz = hdf::calibrate_tsc_ghz();

    Config cfg;

    std::cout << "=== MatchingEngine 专项 Benchmark (Cycles + ns 版) ===\n"
              << "  当前绑核: " << test_core_id << "\n"
              << "  TSC 频率: " << g_tsc_ghz << " GHz\n"
              << "  订单数: " << cfg.numOrders
              << "  证券数: " << cfg.numSecurities
              << "  簿深度: " << cfg.bookDepth << "  热身: " << cfg.warmup
              << "\n";

    benchAddOrder(cfg);
    benchMatchHit(cfg);
    benchMatchMiss(cfg);
    benchCancel(cfg);

    std::cout << "\n=== 完成 ===\n";
    return 0;
}