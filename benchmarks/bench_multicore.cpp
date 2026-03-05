/**
 * @file bench_multicore.cpp
 * @brief 多 Bucket 并行扩展性压测
 *
 * 比较不同 bucket 数量下、多生产者并发提交时的吞吐量变化。
 * 每个 bucket 拥有独立 SecurityCore + 队列 + 消费者线程，
 * 不同证券的订单路由到不同 bucket 后可真正并行执行。
 *
 * 用法:
 *   ./bin/bench_multicore                           # 默认配置
 *   ./bin/bench_multicore --producers 8 --orders 200000
 *   ./bin/bench_multicore --buckets 1,2,4,8 --securities 100
 */

#include "trade_system.h"
#include <algorithm>
#include <atomic>
#include <barrier>
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

using Clock = std::chrono::high_resolution_clock;
using namespace std::chrono;

// ── 配置 ─────────────────────────────────────────────────────
struct Config {
    int totalOrders = 200000;
    int numProducers = 4;
    std::vector<int> bucketCounts = {1, 2, 4};
    int numSecurities = 50;
    int numShareholders = 500;
    int warmupOrders = 2000;
};

static Config parseArgs(int argc, char *argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--orders" && i + 1 < argc)
            cfg.totalOrders = std::atoi(argv[++i]);
        else if (a == "--producers" && i + 1 < argc)
            cfg.numProducers = std::atoi(argv[++i]);
        else if (a == "--buckets" && i + 1 < argc) {
            cfg.bucketCounts.clear();
            std::string list = argv[++i];
            std::istringstream ss(list);
            std::string token;
            while (std::getline(ss, token, ',')) {
                int n = std::atoi(token.c_str());
                if (n > 0)
                    cfg.bucketCounts.push_back(n);
            }
        } else if (a == "--securities" && i + 1 < argc)
            cfg.numSecurities = std::atoi(argv[++i]);
        else if (a == "--shareholders" && i + 1 < argc)
            cfg.numShareholders = std::atoi(argv[++i]);
        else if (a == "--warmup" && i + 1 < argc)
            cfg.warmupOrders = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::cout << "用法: bench_multicore [选项]\n"
                      << "  --orders N          总订单量 (默认 200000)\n"
                      << "  --producers P       生产者线程数 (默认 4)\n"
                      << "  --buckets B         bucket 数列表，逗号分隔 (默认 "
                         "1,2,4)\n"
                      << "  --securities M      证券种类数 (默认 50)\n"
                      << "  --shareholders K    股东数 (默认 500)\n"
                      << "  --warmup W          热身订单数 (默认 2000)\n";
            std::exit(0);
        }
    }
    return cfg;
}

// ── 订单生成 ─────────────────────────────────────────────────
static std::vector<nlohmann::json> generateOrders(const Config &cfg, int count,
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

    std::vector<nlohmann::json> orders;
    orders.reserve(count);
    for (int i = 0; i < count; ++i) {
        nlohmann::json j;
        j["clOrderId"] = "M" + std::to_string(idOffset + i);
        j["market"] = "XSHG";
        j["securityId"] = securities[secDist(rng)];
        j["side"] = sideDist(rng) == 0 ? "B" : "S";
        j["price"] = std::round(priceDist(rng) * 100.0) / 100.0;
        j["qty"] = qtyDist(rng) * 100;
        j["shareholderId"] = shareholders[shDist(rng)];
        orders.push_back(std::move(j));
    }
    return orders;
}

// ── 延时统计 ─────────────────────────────────────────────────
struct LatStats {
    double mean = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
    size_t count = 0;
};

static LatStats calcStats(std::vector<double> &lat) {
    LatStats s{};
    s.count = lat.size();
    if (s.count == 0)
        return s;
    std::sort(lat.begin(), lat.end());
    s.mean = std::accumulate(lat.begin(), lat.end(), 0.0) / s.count;
    auto pct = [&](double p) {
        return lat[std::min(size_t(p / 100.0 * (s.count - 1)), s.count - 1)];
    };
    s.p50 = pct(50);
    s.p95 = pct(95);
    s.p99 = pct(99);
    s.max = lat.back();
    return s;
}

// ── 单轮结果 ─────────────────────────────────────────────────
struct RoundResult {
    int numBuckets;
    int numProducers;
    double elapsed_s;
    double throughput;
    LatStats e2eLat;
    LatStats submitLat;
    size_t execReports;
    size_t rejectReports;
};

// ── 单轮测试：N buckets + P producers ───────────────────────
static RoundResult runRound(const Config &cfg, int numBuckets) {
    hdf::TradeSystem system(numBuckets);

    std::atomic<size_t> matchedCount{0};
    std::atomic<size_t> rejectedCount{0};

    int ordersPerProducer = cfg.totalOrders / cfg.numProducers;
    int actualTotal = ordersPerProducer * cfg.numProducers;

    // 端到端延时追踪
    struct OrderTiming {
        int64_t submit_ns = 0;
        int64_t first_response_ns = 0;
    };
    std::vector<OrderTiming> timings(actualTotal);

    auto parseIdx = [&](const std::string &id) -> int {
        if (id.size() < 2 || id[0] != 'M')
            return -1;
        int x = std::atoi(id.c_str() + 1);
        int idx = x - cfg.warmupOrders;
        return (idx >= 0 && idx < actualTotal) ? idx : -1;
    };

    system.setSendToClient([&](const nlohmann::json &resp) {
        if (resp.contains("execId"))
            matchedCount.fetch_add(1, std::memory_order_relaxed);
        else if (resp.contains("rejectCode"))
            rejectedCount.fetch_add(1, std::memory_order_relaxed);

        std::string orderId = resp.value("clOrderId", "");
        int idx = parseIdx(orderId);
        if (idx >= 0 && timings[idx].first_response_ns == 0) {
            timings[idx].first_response_ns =
                Clock::now().time_since_epoch().count();
        }
    });

    // 热身（同步，单线程）
    auto warmupOrders = generateOrders(cfg, cfg.warmupOrders, 0, 9999);
    for (auto &order : warmupOrders) {
        system.handleOrder(order);
    }
    matchedCount = 0;
    rejectedCount = 0;

    // 启动事件循环（每个 bucket 一个消费者线程）
    system.startEventLoop();

    // 预生成订单
    std::vector<std::vector<nlohmann::json>> producerOrders(cfg.numProducers);
    for (int t = 0; t < cfg.numProducers; ++t) {
        producerOrders[t] =
            generateOrders(cfg, ordersPerProducer,
                           cfg.warmupOrders + t * ordersPerProducer, 42 + t);
    }

    // 每生产者提交延时
    std::vector<std::vector<double>> submitLats(cfg.numProducers);
    for (int t = 0; t < cfg.numProducers; ++t) {
        submitLats[t].reserve(ordersPerProducer);
    }

    std::barrier syncStart(cfg.numProducers + 1);

    std::vector<std::thread> threads;
    threads.reserve(cfg.numProducers);

    for (int t = 0; t < cfg.numProducers; ++t) {
        threads.emplace_back([&, t]() {
            auto &orders = producerOrders[t];
            auto &lats = submitLats[t];

            syncStart.arrive_and_wait();

            for (int i = 0; i < ordersPerProducer; ++i) {
                int globalIdx = t * ordersPerProducer + i;

                auto t0 = Clock::now();
                timings[globalIdx].submit_ns = t0.time_since_epoch().count();

                system.submitOrder(orders[i]);
                auto t1 = Clock::now();

                lats.push_back(duration_cast<nanoseconds>(t1 - t0).count() /
                               1000.0);
            }
        });
    }

    auto wallStart = Clock::now();
    syncStart.arrive_and_wait();

    for (auto &th : threads) {
        th.join();
    }

    system.stopEventLoop();
    auto wallEnd = Clock::now();
    double elapsed_s =
        duration_cast<microseconds>(wallEnd - wallStart).count() / 1e6;

    // 聚合
    std::vector<double> allSubmit, allE2E;
    for (int t = 0; t < cfg.numProducers; ++t) {
        allSubmit.insert(allSubmit.end(), submitLats[t].begin(),
                         submitLats[t].end());
    }
    allE2E.reserve(actualTotal);
    for (int i = 0; i < actualTotal; ++i) {
        if (timings[i].submit_ns > 0 && timings[i].first_response_ns > 0) {
            allE2E.push_back(
                (timings[i].first_response_ns - timings[i].submit_ns) / 1000.0);
        }
    }

    RoundResult r;
    r.numBuckets = numBuckets;
    r.numProducers = cfg.numProducers;
    r.elapsed_s = elapsed_s;
    r.throughput = actualTotal / elapsed_s;
    r.e2eLat = calcStats(allE2E);
    r.submitLat = calcStats(allSubmit);
    r.execReports = matchedCount.load();
    r.rejectReports = rejectedCount.load();
    return r;
}

// ── 输出 ─────────────────────────────────────────────────────
static void printRound(const RoundResult &r) {
    std::cout << "\n──────────────────────────────────────────────────\n";
    std::cout << "  Buckets: " << r.numBuckets
              << "   Producers: " << r.numProducers
              << "   Threads: " << (r.numBuckets + r.numProducers) << "\n";
    std::cout << "  订单数: " << r.e2eLat.count << "   耗时: " << std::fixed
              << std::setprecision(3) << r.elapsed_s << "s"
              << "   成交: " << r.execReports << "   拒绝: " << r.rejectReports
              << "\n";
    std::cout << "  吞吐量: " << std::fixed << std::setprecision(0)
              << r.throughput << " orders/s\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  E2E 延时(us):  p50=" << r.e2eLat.p50
              << "  p95=" << r.e2eLat.p95 << "  p99=" << r.e2eLat.p99
              << "  max=" << r.e2eLat.max << "\n";
    std::cout << "  入队延时(us):  p50=" << r.submitLat.p50
              << "  p95=" << r.submitLat.p95 << "  p99=" << r.submitLat.p99
              << "  max=" << r.submitLat.max << "\n";
}

static void printSummary(const std::vector<RoundResult> &results) {
    std::cout
        << "\n\n╔══════════════════════════════════════════════════════════"
           "══════════════╗\n";
    std::cout << "║  多 Bucket 并行扩展性测试                                  "
                 "            ║\n";
    std::cout
        << "╠═════════╦═══════════╦══════════╦══════════╦══════════╦══════"
           "════════════╣\n";
    std::cout << "║ Buckets ║ Throughput║ E2E-p50  ║ E2E-p99  ║ Enq-p50  "
                 "║  加速比        ║\n";
    std::cout << "║ (消费者)║  (ops/s)  ║   (us)   ║   (us)   ║   (us)   ║  "
                 "(vs 1 bucket) ║\n";
    std::cout
        << "╠═════════╬═══════════╬══════════╬══════════╬══════════╬══════"
           "════════════╣\n";

    double baseline = results.empty() ? 1.0 : results[0].throughput;
    for (auto &r : results) {
        double speedup = r.throughput / baseline;
        std::cout << std::fixed;
        std::cout << "║ " << std::setw(7) << r.numBuckets << " ║ "
                  << std::setw(9) << std::setprecision(0) << r.throughput
                  << " ║ " << std::setw(8) << std::setprecision(2)
                  << r.e2eLat.p50 << " ║ " << std::setw(8)
                  << std::setprecision(2) << r.e2eLat.p99 << " ║ "
                  << std::setw(8) << std::setprecision(2) << r.submitLat.p50
                  << " ║ " << std::setw(14) << std::setprecision(2) << speedup
                  << "x ║\n";
    }
    std::cout
        << "╚═════════╩═══════════╩══════════╩══════════╩══════════╩══════"
           "════════════╝\n";
}

// ── main ─────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    Config cfg = parseArgs(argc, argv);

    unsigned int hwThreads = std::thread::hardware_concurrency();
    std::cout << "=== 多 Bucket 并行扩展性 Benchmark ===\n"
              << "  总订单: " << cfg.totalOrders
              << "  生产者: " << cfg.numProducers
              << "  证券数: " << cfg.numSecurities
              << "  股东数: " << cfg.numShareholders
              << "  热身: " << cfg.warmupOrders << "\n"
              << "  硬件线程数: " << hwThreads << "  测试 bucket 数: ";
    for (size_t i = 0; i < cfg.bucketCounts.size(); ++i) {
        if (i > 0)
            std::cout << ",";
        std::cout << cfg.bucketCounts[i];
    }
    std::cout << "\n";

    std::vector<RoundResult> results;
    for (int nBuckets : cfg.bucketCounts) {
        std::cout << "\n>>> " << nBuckets << " bucket(s), " << cfg.numProducers
                  << " producers..." << std::flush;
        auto r = runRound(cfg, nBuckets);
        results.push_back(r);
        printRound(r);
    }

    printSummary(results);
    return 0;
}
