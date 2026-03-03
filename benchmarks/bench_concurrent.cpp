/**
 * @file bench_concurrent.cpp
 * @brief 并发压测基线 —— 多线程模拟多客户端并发提交订单
 *
 * 支持两种模式：
 *   mutex  — 外部 std::mutex 保护（改造前基线）
 *   queue  — MPSC 命令队列 + 单消费者线程（改造后）
 *
 * 用法:
 *   ./bin/bench_concurrent --mode mutex   # 测 mutex 方案
 *   ./bin/bench_concurrent --mode queue   # 测 MPSC 队列方案
 *   ./bin/bench_concurrent --mode both    # 两种都跑，对比输出
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
    int warmupOrders = 2000;   // 热身订单数（单线程执行）
    std::string mode = "both"; // "mutex", "queue", "both"
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
        else if (a == "--mode" && i + 1 < argc)
            cfg.mode = argv[++i];
        else if (a == "--help" || a == "-h") {
            std::cout
                << "用法: bench_concurrent [选项]\n"
                << "  --orders N         总订单量 (默认 100000)\n"
                << "  --threads T        线程数列表，逗号分隔 (默认 1,2,4,8)\n"
                << "  --securities M     证券种类数 (默认 10)\n"
                << "  --shareholders K   股东数 (默认 500)\n"
                << "  --warmup W         热身订单数 (默认 2000)\n"
                << "  --mode M           测试模式: mutex / queue / both (默认 "
                   "both)\n";
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
    std::string mode; // "mutex" or "queue"
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
    r.mode = "mutex";
    r.elapsed_s = elapsed_s;
    r.aggregateThroughput = totalOps / elapsed_s;
    r.totalLat = calcStats(allTotal);
    r.waitLat = calcStats(allWait);
    r.processLat = calcStats(allProcess);
    r.execReports = matchedCount.load();
    r.rejectReports = rejectedCount.load();
    return r;
}

// ── 单轮并发测试（MPSC 队列模式） ──────────────────────────
// 多线程通过 submitOrder 投递到命令队列，单消费者线程串行处理
// 测量的是"从 submitOrder 调用到回调被触发"的端到端时间
static RoundResult runRoundQueue(const Config &cfg, int numThreads) {
    hdf::TradeSystem system;
    std::atomic<size_t> matchedCount{0};
    std::atomic<size_t> rejectedCount{0};
    std::atomic<size_t> completedCount{0};

    system.setSendToClient([&](const nlohmann::json &resp) {
        if (resp.contains("execId"))
            matchedCount.fetch_add(1, std::memory_order_relaxed);
        else if (resp.contains("rejectCode"))
            rejectedCount.fetch_add(1, std::memory_order_relaxed);
        completedCount.fetch_add(1, std::memory_order_relaxed);
    });

    // 热身（单线程直接调用，不经队列）
    auto warmupOrders = generateOrders(cfg, cfg.warmupOrders, 0, 9999);
    for (auto &order : warmupOrders) {
        system.handleOrder(order);
    }
    matchedCount = 0;
    rejectedCount = 0;
    completedCount = 0;

    // 启动事件循环
    system.startEventLoop();

    // 为每个线程预生成订单
    int ordersPerThread = cfg.totalOrders / numThreads;
    int actualTotal = ordersPerThread * numThreads;
    std::vector<std::vector<nlohmann::json>> threadOrders(numThreads);
    for (int t = 0; t < numThreads; ++t) {
        threadOrders[t] =
            generateOrders(cfg, ordersPerThread,
                           cfg.warmupOrders + t * ordersPerThread, 42 + t);
    }

    // 每线程只测量提交延时（入队时间）
    std::vector<std::vector<double>> submitLats(numThreads);
    for (int t = 0; t < numThreads; ++t) {
        submitLats[t].reserve(ordersPerThread);
    }

    std::barrier syncStart(numThreads + 1);

    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&, t]() {
            auto &orders = threadOrders[t];
            auto &lats = submitLats[t];

            syncStart.arrive_and_wait();

            for (auto &order : orders) {
                auto t0 = Clock::now();
                system.submitOrder(order);
                auto t1 = Clock::now();

                double submit_us =
                    duration_cast<nanoseconds>(t1 - t0).count() / 1000.0;
                lats.push_back(submit_us);
            }
        });
    }

    auto wallStart = Clock::now();
    syncStart.arrive_and_wait();

    // 等待所有生产者完成
    for (auto &th : threads) {
        th.join();
    }
    auto submitEnd = Clock::now();

    // 等待消费者处理完所有命令（通过轮询 queueDepth）
    while (system.queueDepth() > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    // 再等一小段确保最后一批处理完毕
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto wallEnd = Clock::now();
    double elapsed_s =
        duration_cast<microseconds>(wallEnd - wallStart).count() / 1e6;
    double submitElapsed =
        duration_cast<microseconds>(submitEnd - wallStart).count() / 1e6;

    system.stopEventLoop();

    // 聚合提交延时
    std::vector<double> allSubmit;
    for (int t = 0; t < numThreads; ++t) {
        allSubmit.insert(allSubmit.end(), submitLats[t].begin(),
                         submitLats[t].end());
    }

    RoundResult r;
    r.numThreads = numThreads;
    r.mode = "queue";
    r.elapsed_s = elapsed_s;
    r.aggregateThroughput = actualTotal / elapsed_s;
    r.totalLat = calcStats(allSubmit); // 对 queue 模式，total = 提交延时
    // wait 和 process 不适用于 queue 模式，设为提交延时和 0
    r.waitLat = calcStats(allSubmit);
    LatStats zeroStats{};
    zeroStats.count = allSubmit.size();
    r.processLat = zeroStats;
    r.execReports = matchedCount.load();
    r.rejectReports = rejectedCount.load();
    return r;
}

// ── 输出 ────────────────────────────────────────────────────
static void printRound(const RoundResult &r) {
    std::cout << "\n==========================================================="
                 "=====\n";
    std::cout << "  [" << r.mode << "] 线程数: " << r.numThreads
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
    if (r.mode == "mutex") {
        printLatStats(r.totalLat, "端到端 (含等锁)");
        printLatStats(r.waitLat, "等锁时间        ");
        printLatStats(r.processLat, "锁内处理时间    ");
    } else {
        printLatStats(r.totalLat, "提交延时 (入队) ");
    }
    std::cout
        << "================================================================\n";
}

static void printSummaryTable(const std::vector<RoundResult> &results,
                              const std::string &title) {
    std::cout << "\n\n╔════════════════════════════════════════════════════════"
                 "═══════╗\n";
    std::cout << "║  " << std::left << std::setw(61) << title << "║\n";
    std::cout << "╠════════╦═══════════╦══════════╦══════════╦══════════╦══════"
                 "═══╣\n";
    std::cout
        << "║ Threads║ Throughput║ Lat-p50  ║ Lat-p99  ║ Wait-p50 ║Wait-p99║\n";
    std::cout
        << "║        ║  (ops/s)  ║   (us)   ║   (us)   ║   (us)   ║  (us)  ║\n";
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
    std::cout << "=== 并发压测 Benchmark ===\n"
              << "  总订单: " << cfg.totalOrders
              << "  证券数: " << cfg.numSecurities
              << "  股东数: " << cfg.numShareholders
              << "  热身: " << cfg.warmupOrders << "  模式: " << cfg.mode
              << "\n  硬件线程数: " << hwThreads << "  测试线程数: ";
    for (size_t i = 0; i < cfg.threadCounts.size(); ++i) {
        if (i > 0)
            std::cout << ",";
        std::cout << cfg.threadCounts[i];
    }
    std::cout << "\n";

    bool runMutex = (cfg.mode == "mutex" || cfg.mode == "both");
    bool runQueue = (cfg.mode == "queue" || cfg.mode == "both");

    std::vector<RoundResult> mutexResults, queueResults;

    if (runMutex) {
        std::cout
            << "\n━━━ Mutex 模式 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        for (int nThreads : cfg.threadCounts) {
            std::cout << "\n>>> [mutex] " << nThreads << " 线程..."
                      << std::flush;
            auto r = runRound(cfg, nThreads);
            mutexResults.push_back(r);
            printRound(r);
        }
        printSummaryTable(mutexResults, "Mutex 模式 — 并发扩展性");
    }

    if (runQueue) {
        std::cout
            << "\n━━━ MPSC Queue 模式 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        for (int nThreads : cfg.threadCounts) {
            std::cout << "\n>>> [queue] " << nThreads << " 线程..."
                      << std::flush;
            auto r = runRoundQueue(cfg, nThreads);
            queueResults.push_back(r);
            printRound(r);
        }
        printSummaryTable(queueResults, "MPSC Queue 模式 — 并发扩展性");
    }

    // 对比总结
    if (runMutex && runQueue && !mutexResults.empty() &&
        !queueResults.empty()) {
        std::cout
            << "\n\n━━━ Mutex vs Queue 对比 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        std::cout << std::fixed << std::setprecision(0);
        for (size_t i = 0; i < mutexResults.size() && i < queueResults.size();
             ++i) {
            auto &m = mutexResults[i];
            auto &q = queueResults[i];
            double speedup = q.aggregateThroughput / m.aggregateThroughput;
            std::cout << "  " << m.numThreads << " 线程: "
                      << "mutex=" << m.aggregateThroughput << " ops/s, "
                      << "queue=" << q.aggregateThroughput << " ops/s, "
                      << std::setprecision(2) << speedup << "x\n"
                      << std::setprecision(0);
        }
    }

    return 0;
}
