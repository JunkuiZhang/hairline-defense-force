/**
 * @file bench_concurrent.cpp
 * @brief 并发压测基线 —— 多线程模拟多客户端并发提交订单
 *
 * 测试场景：
 *   N 个线程同时向同一个 TradeSystem 提交订单，用 std::mutex 保护
 *   （模拟最朴素的加锁方案），测量：
 *     - 聚合吞吐量随线程数的变化
 *     - 各线程的延时分位数（含排队等锁时间）
 *     - 锁等待时间 vs 实际处理时间 的拆分
 *
 *   改造为 MPSC 队列架构后，重新跑此 benchmark 对比。
 *
 * 用法:
 *   cmake --build build --target bench_concurrent
 *   ./bin/bench_concurrent [--orders N] [--threads T] [--securities M]
 */

#include "trade_system.h"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::high_resolution_clock;
using namespace std::chrono;

// ── 配置 ────────────────────────────────────────────────────
struct Config {
    int totalOrders = 100000;                     // 总订单数（均分到各线程）
    std::vector<int> threadCounts = {1, 2, 4, 8}; // 要测试的线程数
    int numSecurities = 10;
    int numShareholders = 500;
    int warmupOrders = 2000; // 热身订单数（单线程执行）
};

static Config parseArgs(int argc, char *argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--orders" && i + 1 < argc)
            cfg.totalOrders = std::atoi(argv[++i]);
        else if (a == "--threads" && i + 1 < argc) {
            // 支持逗号分隔的线程数列表，如 --threads 1,2,4,8,16
            cfg.threadCounts.clear();
            std::string list = argv[++i];
            std::istringstream ss(list);
            std::string token;
            while (std::getline(ss, token, ',')) {
                cfg.threadCounts.push_back(std::atoi(token.c_str()));
            }
        } else if (a == "--securities" && i + 1 < argc)
            cfg.numSecurities = std::atoi(argv[++i]);
        else if (a == "--shareholders" && i + 1 < argc)
            cfg.numShareholders = std::atoi(argv[++i]);
        else if (a == "--warmup" && i + 1 < argc)
            cfg.warmupOrders = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::cout
                << "用法: bench_concurrent [选项]\n"
                << "  --orders N         总订单量 (默认 100000)\n"
                << "  --threads T        线程数列表，逗号分隔 (默认 1,2,4,8)\n"
                << "  --securities M     证券种类数 (默认 10)\n"
                << "  --shareholders K   股东数 (默认 500)\n"
                << "  --warmup W         热身订单数 (默认 2000)\n";
            std::exit(0);
        }
    }
    return cfg;
}

// ── 订单生成器 ──────────────────────────────────────────────
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
        j["clOrderId"] = "T" + std::to_string(idOffset + i);
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

// ── 每线程的延时记录 ────────────────────────────────────────
struct ThreadLatency {
    std::vector<double> total_us;   // 端到端（含等锁）
    std::vector<double> wait_us;    // 等锁时间
    std::vector<double> process_us; // 实际处理时间（锁内）
};

// ── 延时统计 ────────────────────────────────────────────────
struct LatStats {
    double mean, p50, p95, p99, max;
    size_t count;
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

static void printLatStats(const LatStats &s, const std::string &label) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "    " << label << ":  mean=" << s.mean << "  p50=" << s.p50
              << "  p95=" << s.p95 << "  p99=" << s.p99 << "  max=" << s.max
              << " us\n";
}

// ── 单轮并发测试 ───────────────────────────────────────────
struct RoundResult {
    int numThreads;
    double elapsed_s;
    double aggregateThroughput;
    LatStats totalLat;   // 聚合所有线程的端到端延时
    LatStats waitLat;    // 聚合所有线程的等锁延时
    LatStats processLat; // 聚合所有线程的锁内处理延时
    size_t execReports;
    size_t rejectReports;
};

static RoundResult runRound(const Config &cfg, int numThreads) {
    // 1. 初始化 TradeSystem（每轮独立实例，保证订单簿从空开始）
    hdf::TradeSystem system;
    std::atomic<size_t> matchedCount{0};
    std::atomic<size_t> rejectedCount{0};

    system.setSendToClient([&](const nlohmann::json &resp) {
        if (resp.contains("execId"))
            matchedCount.fetch_add(1, std::memory_order_relaxed);
        else if (resp.contains("rejectCode"))
            rejectedCount.fetch_add(1, std::memory_order_relaxed);
    });

    // 2. 热身（单线程，不计时）
    auto warmupOrders = generateOrders(cfg, cfg.warmupOrders, 0, 9999);
    for (auto &order : warmupOrders) {
        system.handleOrder(order);
    }
    matchedCount = 0;
    rejectedCount = 0;

    // 3. 为每个线程预生成订单
    int ordersPerThread = cfg.totalOrders / numThreads;
    std::vector<std::vector<nlohmann::json>> threadOrders(numThreads);
    for (int t = 0; t < numThreads; ++t) {
        threadOrders[t] =
            generateOrders(cfg, ordersPerThread,
                           cfg.warmupOrders + t * ordersPerThread, 42 + t);
    }

    // 4. 准备延时收集器
    std::vector<ThreadLatency> threadLats(numThreads);
    for (int t = 0; t < numThreads; ++t) {
        threadLats[t].total_us.reserve(ordersPerThread);
        threadLats[t].wait_us.reserve(ordersPerThread);
        threadLats[t].process_us.reserve(ordersPerThread);
    }

    // 5. 用 barrier 确保所有线程同时开跑
    std::mutex systemMutex;                 // 保护 TradeSystem 的互斥锁
    std::barrier syncStart(numThreads + 1); // +1 for main thread

    // 6. 启动工作线程
    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&, t]() {
            auto &orders = threadOrders[t];
            auto &lat = threadLats[t];

            syncStart.arrive_and_wait(); // 等待所有线程就绪

            for (auto &order : orders) {
                auto t0 = Clock::now();

                systemMutex.lock();
                auto t1 = Clock::now(); // 拿到锁

                system.handleOrder(order);

                auto t2 = Clock::now(); // 处理完毕
                systemMutex.unlock();

                double total =
                    duration_cast<nanoseconds>(t2 - t0).count() / 1000.0;
                double wait =
                    duration_cast<nanoseconds>(t1 - t0).count() / 1000.0;
                double proc =
                    duration_cast<nanoseconds>(t2 - t1).count() / 1000.0;

                lat.total_us.push_back(total);
                lat.wait_us.push_back(wait);
                lat.process_us.push_back(proc);
            }
        });
    }

    // 7. 主线程发令 + 计时
    auto wallStart = Clock::now();
    syncStart.arrive_and_wait(); // 发令枪

    for (auto &th : threads) {
        th.join();
    }
    auto wallEnd = Clock::now();
    double elapsed_s =
        duration_cast<microseconds>(wallEnd - wallStart).count() / 1e6;

    // 8. 聚合所有线程的延时数据
    std::vector<double> allTotal, allWait, allProcess;
    size_t totalOps = 0;
    for (int t = 0; t < numThreads; ++t) {
        totalOps += threadLats[t].total_us.size();
        allTotal.insert(allTotal.end(), threadLats[t].total_us.begin(),
                        threadLats[t].total_us.end());
        allWait.insert(allWait.end(), threadLats[t].wait_us.begin(),
                       threadLats[t].wait_us.end());
        allProcess.insert(allProcess.end(), threadLats[t].process_us.begin(),
                          threadLats[t].process_us.end());
    }

    RoundResult r;
    r.numThreads = numThreads;
    r.elapsed_s = elapsed_s;
    r.aggregateThroughput = totalOps / elapsed_s;
    r.totalLat = calcStats(allTotal);
    r.waitLat = calcStats(allWait);
    r.processLat = calcStats(allProcess);
    r.execReports = matchedCount.load();
    r.rejectReports = rejectedCount.load();
    return r;
}

// ── 输出 ────────────────────────────────────────────────────
static void printRound(const RoundResult &r) {
    std::cout << "\n==========================================================="
                 "=====\n";
    std::cout << "  线程数: " << r.numThreads
              << "    总订单: " << r.totalLat.count
              << "    耗时: " << std::fixed << std::setprecision(3)
              << r.elapsed_s << "s\n";
    std::cout << "  成交回报: " << r.execReports
              << "    拒绝回报: " << r.rejectReports << "\n";
    std::cout
        << "----------------------------------------------------------------\n";
    std::cout << std::fixed << std::setprecision(0);
    std::cout << "  聚合吞吐量: " << r.aggregateThroughput << " orders/s\n";
    std::cout
        << "----------------------------------------------------------------\n";
    std::cout << "  延时 (us):\n";
    printLatStats(r.totalLat, "端到端 (含等锁)");
    printLatStats(r.waitLat, "等锁时间        ");
    printLatStats(r.processLat, "锁内处理时间    ");
    std::cout
        << "================================================================\n";
}

static void printSummaryTable(const std::vector<RoundResult> &results) {
    std::cout << "\n\n╔════════════════════════════════════════════════════════"
                 "═══════╗\n";
    std::cout << "║                     并发扩展性总结                         "
                 "   ║\n";
    std::cout << "╠════════╦═══════════╦══════════╦══════════╦══════════╦══════"
                 "═══╣\n";
    std::cout << "║ Threads║ Throughput║ Total-p50║ Total-p99║ Wait-p50 ║ "
                 "Wait-p99║\n";
    std::cout << "║        ║  (ops/s)  ║   (us)   ║   (us)   ║   (us)   ║  "
                 "(us)   ║\n";
    std::cout << "╠════════╬═══════════╬══════════╬══════════╬══════════╬══════"
                 "═══╣\n";

    for (auto &r : results) {
        std::cout << std::fixed;
        std::cout << "║ " << std::setw(6) << r.numThreads << " ║ "
                  << std::setw(9) << std::setprecision(0)
                  << r.aggregateThroughput << " ║ " << std::setw(8)
                  << std::setprecision(2) << r.totalLat.p50 << " ║ "
                  << std::setw(8) << std::setprecision(2) << r.totalLat.p99
                  << " ║ " << std::setw(8) << std::setprecision(2)
                  << r.waitLat.p50 << " ║ " << std::setw(7)
                  << std::setprecision(2) << r.waitLat.p99 << " ║\n";
    }
    std::cout << "╚════════╩═══════════╩══════════╩══════════╩══════════╩══════"
                 "═══╝\n";

    // 计算扩展效率
    if (results.size() >= 2) {
        double baseline = results[0].aggregateThroughput;
        std::cout << "\n  扩展效率 (相对 " << results[0].numThreads
                  << " 线程):\n";
        for (size_t i = 1; i < results.size(); ++i) {
            double speedup = results[i].aggregateThroughput / baseline;
            double efficiency =
                speedup /
                (double(results[i].numThreads) / results[0].numThreads) * 100.0;
            std::cout << std::fixed << std::setprecision(2);
            std::cout << "    " << results[i].numThreads << " 线程: " << speedup
                      << "x 加速比, " << efficiency << "% 效率\n";
        }
    }
}

// ── main ────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    Config cfg = parseArgs(argc, argv);

    unsigned int hwThreads = std::thread::hardware_concurrency();
    std::cout << "=== 并发压测基线 Benchmark ===\n"
              << "  总订单: " << cfg.totalOrders
              << "  证券数: " << cfg.numSecurities
              << "  股东数: " << cfg.numShareholders
              << "  热身: " << cfg.warmupOrders
              << "\n  硬件线程数: " << hwThreads << "  测试线程数: ";
    for (size_t i = 0; i < cfg.threadCounts.size(); ++i) {
        if (i > 0)
            std::cout << ",";
        std::cout << cfg.threadCounts[i];
    }
    std::cout << "\n";

    std::vector<RoundResult> results;
    for (int nThreads : cfg.threadCounts) {
        std::cout << "\n>>> 正在测试 " << nThreads << " 线程..." << std::flush;
        auto r = runRound(cfg, nThreads);
        results.push_back(r);
        printRound(r);
    }

    printSummaryTable(results);
    return 0;
}
