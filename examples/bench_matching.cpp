/**
 * @file bench_matching.cpp
 * @brief 撮合引擎专项 Benchmark
 *
 * 绕过 JSON 解析、风控、日志，直接用 Order 结构体驱动 MatchingEngine，
 * 隔离测量 addOrder / match 各自的延时和吞吐。
 *
 * 用法:
 *   cmake --build build --target bench_matching
 *   ./bin/bench_matching [--orders N] [--securities M] [--depth D]
 */

#include "matching_engine.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using Clock = std::chrono::high_resolution_clock;
using namespace std::chrono;

// ── 配置 ────────────────────────────────────────────────────
struct Config {
    int numOrders = 100000; // 测量的订单数
    int numSecurities = 10; // 证券种类
    int bookDepth = 5000;   // 每只证券预挂单深度（买+卖各一半）
    int warmup = 1000;      // 热身订单数
};

static Config parseArgs(int argc, char *argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--orders" && i + 1 < argc)
            cfg.numOrders = std::atoi(argv[++i]);
        else if (a == "--securities" && i + 1 < argc)
            cfg.numSecurities = std::atoi(argv[++i]);
        else if (a == "--depth" && i + 1 < argc)
            cfg.bookDepth = std::atoi(argv[++i]);
        else if (a == "--warmup" && i + 1 < argc)
            cfg.warmup = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::cout << "用法: bench_matching [选项]\n"
                      << "  --orders N      测量订单数 (默认 100000)\n"
                      << "  --securities M  证券种类 (默认 10)\n"
                      << "  --depth D       每只证券预挂单深度 (默认 5000)\n"
                      << "  --warmup W      热身订单数 (默认 1000)\n";
            std::exit(0);
        }
    }
    return cfg;
}

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
    double mean_us, p50_us, p95_us, p99_us, max_us;
    double throughput;
    size_t count;
};

static LatStats calcStats(std::vector<double> &lat, double elapsed_s) {
    LatStats s{};
    s.count = lat.size();
    if (s.count == 0)
        return s;
    std::sort(lat.begin(), lat.end());
    s.mean_us = std::accumulate(lat.begin(), lat.end(), 0.0) / s.count;
    auto pct = [&](double p) { return lat[size_t(p / 100.0 * (s.count - 1))]; };
    s.p50_us = pct(50);
    s.p95_us = pct(95);
    s.p99_us = pct(99);
    s.max_us = lat.back();
    s.throughput = s.count / elapsed_s;
    return s;
}

static void printStats(const LatStats &s, const std::string &label) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  [" << label << "]  N=" << s.count
              << "  tput=" << s.throughput << " op/s\n"
              << "    mean=" << s.mean_us << " us  "
              << "p50=" << s.p50_us << "  "
              << "p95=" << s.p95_us << "  "
              << "p99=" << s.p99_us << "  "
              << "max=" << s.max_us << " us\n";
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

    // 热身
    for (int i = 0; i < cfg.warmup; ++i)
        engine.addOrder(orders[i]);

    // 测量
    std::vector<double> lat;
    lat.reserve(cfg.numOrders);
    auto wall0 = Clock::now();
    for (int i = cfg.warmup; i < cfg.warmup + cfg.numOrders; ++i) {
        auto t0 = Clock::now();
        engine.addOrder(orders[i]);
        auto t1 = Clock::now();
        lat.push_back(duration_cast<nanoseconds>(t1 - t0).count() / 1000.0);
    }
    auto wall1 = Clock::now();
    double elapsed = duration_cast<microseconds>(wall1 - wall0).count() / 1e6;
    auto s = calcStats(lat, elapsed);
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

    // 填充订单簿：每只证券挂 depth/2 买单 + depth/2 卖单
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

    // 生成可成交的测试订单（价格覆盖簿中的范围，保证能撮合）
    std::vector<hdf::Order> testOrders;
    testOrders.reserve(cfg.warmup + cfg.numOrders);
    for (int i = 0; i < cfg.warmup + cfg.numOrders; ++i) {
        int sec = secDist(rng);
        // 交替产生买卖：买单出高价，卖单出低价，确保能匹配
        bool isBuy = (i % 2 == 0);
        double price = isBuy ? 50.0 : 5.0; // 激进价格保证成交
        uint32_t qty = 100;                // 小量，只匹配一笔
        auto side = isBuy ? hdf::Side::BUY : hdf::Side::SELL;
        testOrders.push_back(makeOrder(id++, secIds[sec], side, price, qty));
    }

    // 热身
    for (int i = 0; i < cfg.warmup; ++i)
        engine.match(testOrders[i]);

    // 测量
    std::vector<double> lat;
    lat.reserve(cfg.numOrders);
    size_t totalExecs = 0;
    auto wall0 = Clock::now();
    for (int i = cfg.warmup; i < cfg.warmup + cfg.numOrders; ++i) {
        auto t0 = Clock::now();
        auto result = engine.match(testOrders[i]);
        auto t1 = Clock::now();
        lat.push_back(duration_cast<nanoseconds>(t1 - t0).count() / 1000.0);
        totalExecs += result.executions.size();
        // 剩余量入簿（维持簿深度）
        if (result.remainingQty > 0) {
            hdf::Order rem = testOrders[i];
            rem.qty = result.remainingQty;
            engine.addOrder(rem);
        }
    }
    auto wall1 = Clock::now();
    double elapsed = duration_cast<microseconds>(wall1 - wall0).count() / 1e6;
    auto s = calcStats(lat, elapsed);
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

    // 填充订单簿：只挂买单（价格 10~20）
    int id = 0;
    for (int sec = 0; sec < cfg.numSecurities; ++sec) {
        for (int d = 0; d < cfg.bookDepth; ++d) {
            double price = 10.0 + d * 0.01;
            engine.addOrder(
                makeOrder(id++, secIds[sec], hdf::Side::BUY, price, 100));
        }
    }
    std::cout << "  订单簿已填充 " << id << " 笔 (仅买单)\n";

    // 测试订单：卖价 > 所有买价，不会成交，但 match() 仍需遍历检查
    std::vector<hdf::Order> testOrders;
    testOrders.reserve(cfg.warmup + cfg.numOrders);
    for (int i = 0; i < cfg.warmup + cfg.numOrders; ++i) {
        int sec = secDist(rng);
        testOrders.push_back(
            makeOrder(id++, secIds[sec], hdf::Side::SELL, 999.0, 100));
    }

    // 热身
    for (int i = 0; i < cfg.warmup; ++i)
        engine.match(testOrders[i]);

    // 测量
    std::vector<double> lat;
    lat.reserve(cfg.numOrders);
    auto wall0 = Clock::now();
    for (int i = cfg.warmup; i < cfg.warmup + cfg.numOrders; ++i) {
        auto t0 = Clock::now();
        auto result = engine.match(testOrders[i]);
        auto t1 = Clock::now();
        lat.push_back(duration_cast<nanoseconds>(t1 - t0).count() / 1000.0);
    }
    auto wall1 = Clock::now();
    double elapsed = duration_cast<microseconds>(wall1 - wall0).count() / 1e6;
    auto s = calcStats(lat, elapsed);
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

    // 先填充订单簿
    int total = cfg.warmup + cfg.numOrders;
    std::vector<std::string> orderIds;
    orderIds.reserve(total);
    for (int i = 0; i < total; ++i) {
        auto side = sideDist(rng) == 0 ? hdf::Side::BUY : hdf::Side::SELL;
        double price = std::round(priceDist(rng) * 100.0) / 100.0;
        auto o = makeOrder(i, secIds[secDist(rng)], side, price, 100);
        engine.addOrder(o);
        orderIds.push_back(o.clOrderId);
    }

    // 打乱顺序（模拟随机撤单）
    std::shuffle(orderIds.begin(), orderIds.end(), rng);

    // 热身
    for (int i = 0; i < cfg.warmup; ++i)
        engine.cancelOrder(orderIds[i]);

    // 测量
    std::vector<double> lat;
    lat.reserve(cfg.numOrders);
    auto wall0 = Clock::now();
    for (int i = cfg.warmup; i < total; ++i) {
        auto t0 = Clock::now();
        engine.cancelOrder(orderIds[i]);
        auto t1 = Clock::now();
        lat.push_back(duration_cast<nanoseconds>(t1 - t0).count() / 1000.0);
    }
    auto wall1 = Clock::now();
    double elapsed = duration_cast<microseconds>(wall1 - wall0).count() / 1e6;
    auto s = calcStats(lat, elapsed);
    printStats(s, "cancelOrder");
}

// ── main ────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    Config cfg = parseArgs(argc, argv);

    std::cout << "=== MatchingEngine 专项 Benchmark ===\n"
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
