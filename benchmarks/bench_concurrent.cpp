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
                int n = std::atoi(token.c_str());
                if (n > 0)
                    cfg.threadCounts.push_back(n);
            }
            if (cfg.threadCounts.empty()) {
                std::cerr << "错误: --threads 至少需要一个 >0 的线程数\n";
                std::exit(1);
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

static void printLatStats(const LatStats &s, const std::string &label) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "    " << label << ":  mean=" << s.mean << "  p50=" << s.p50
              << "  p95=" << s.p95 << "  p99=" << s.p99 << "  max=" << s.max
              << " us\n";
}

// ── 测试结果 ────────────────────────────────────────────────
struct RoundResult {
    int numThreads;
    std::string mode; // "mutex" or "queue"
    double elapsed_s;
    double aggregateThroughput;
    LatStats totalLat;   // mutex: 端到端(含等锁); queue: 端到端(提交→首次回调)
    LatStats waitLat;    // mutex: 等锁延时
    LatStats processLat; // mutex: 锁内处理延时
    LatStats submitLat;  // queue: 入队延时
    size_t execReports;
    size_t rejectReports;
};

// ── 单轮并发测试（Mutex 模式） ──────────────────────────────
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
    int actualTotal = ordersPerThread * numThreads;
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
// 多生产者线程通过 submitOrder 投递到命令队列，单消费者线程串行处理。
// 测量两种延时：
//   submitLat  — 入队延时（生产者侧）
//   totalLat   — 端到端延时（从 submitOrder 到首次回调被触发）
static RoundResult runRoundQueue(const Config &cfg, int numThreads) {
    hdf::TradeSystem system;
    std::atomic<size_t> matchedCount{0};
    std::atomic<size_t> rejectedCount{0};

    // 提前计算订单数（callback lambda 需要捕获 actualTotal）
    int ordersPerThread = cfg.totalOrders / numThreads;
    int actualTotal = ordersPerThread * numThreads;

    // ── 端到端延时追踪 ──────────────────────────────────────
    // 每个订单一个 slot，按 clOrderId 中的序号索引。
    // 生产者写 submit_ns（在 submitOrder 前），消费者写 first_response_ns。
    // 两者之间通过 MPSC 队列的 mutex 保证 happens-before。
    struct OrderTiming {
        int64_t submit_ns = 0;
        int64_t first_response_ns = 0;
    };
    std::vector<OrderTiming> timings(actualTotal);

    // clOrderId = "T{warmupOrders + globalIdx}" → 解析回 globalIdx
    auto parseIdx = [&](const std::string &id) -> int {
        if (id.size() < 2 || id[0] != 'T')
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

        // 记录该订单的首次回调时间（用于端到端延时）
        // 一个订单可能触发多次回调（maker + taker exec report），只记首次
        std::string orderId = resp.value("clOrderId", "");
        int idx = parseIdx(orderId);
        if (idx >= 0 && timings[idx].first_response_ns == 0) {
            timings[idx].first_response_ns =
                Clock::now().time_since_epoch().count();
        }
    });

    // 热身（单线程直接调用，不经队列）
    auto warmupOrders = generateOrders(cfg, cfg.warmupOrders, 0, 9999);
    for (auto &order : warmupOrders) {
        system.handleOrder(order);
    }
    matchedCount = 0;
    rejectedCount = 0;

    // 启动事件循环（消费者线程）
    system.startEventLoop();

    // 为每个线程预生成订单
    std::vector<std::vector<nlohmann::json>> threadOrders(numThreads);
    for (int t = 0; t < numThreads; ++t) {
        threadOrders[t] =
            generateOrders(cfg, ordersPerThread,
                           cfg.warmupOrders + t * ordersPerThread, 42 + t);
    }

    // 每线程测量提交延时（入队时间）
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

            for (int i = 0; i < ordersPerThread; ++i) {
                int globalIdx = t * ordersPerThread + i;

                auto t0 = Clock::now();
                // 记录提交时间戳（消费者通过 queue mutex happens-before 读取）
                timings[globalIdx].submit_ns = t0.time_since_epoch().count();

                system.submitOrder(orders[i]);
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

    // stopEventLoop() 会等待队列排空并处理完全部命令后才返回，
    // 保证所有订单被消费且回调已执行，wallEnd 时间准确。
    system.stopEventLoop();
    auto wallEnd = Clock::now();
    double elapsed_s =
        duration_cast<microseconds>(wallEnd - wallStart).count() / 1e6;

    // ── 聚合提交延时 ────────────────────────────────────────
    std::vector<double> allSubmit;
    for (int t = 0; t < numThreads; ++t) {
        allSubmit.insert(allSubmit.end(), submitLats[t].begin(),
                         submitLats[t].end());
    }

    // ── 计算端到端延时（submit → 首次回调） ─────────────────
    std::vector<double> allE2E;
    allE2E.reserve(actualTotal);
    for (int i = 0; i < actualTotal; ++i) {
        if (timings[i].submit_ns > 0 && timings[i].first_response_ns > 0) {
            double e2e_us =
                (timings[i].first_response_ns - timings[i].submit_ns) / 1000.0;
            allE2E.push_back(e2e_us);
        }
    }

    RoundResult r;
    r.numThreads = numThreads;
    r.mode = "queue";
    r.elapsed_s = elapsed_s;
    r.aggregateThroughput = actualTotal / elapsed_s;
    r.totalLat = calcStats(allE2E);     // 端到端 = 提交 → 首次回调
    r.submitLat = calcStats(allSubmit); // 入队延时
    // wait / process 不适用于 queue 模式
    r.execReports = matchedCount.load();
    r.rejectReports = rejectedCount.load();
    return r;
}

// ── 输出 ────────────────────────────────────────────────────
static void printRound(const RoundResult &r) {
    std::cout << "\n==========================================================="
                 "=====\n";
    if (r.mode == "queue") {
        // queue 模式有额外的消费者线程，标注清楚
        std::cout << "  [" << r.mode << "] 生产者线程: " << r.numThreads
                  << " (+1 consumer)"
                  << "    总订单: " << r.totalLat.count
                  << "    耗时: " << std::fixed << std::setprecision(3)
                  << r.elapsed_s << "s\n";
    } else {
        std::cout << "  [" << r.mode << "] 线程数: " << r.numThreads
                  << "    总订单: " << r.totalLat.count
                  << "    耗时: " << std::fixed << std::setprecision(3)
                  << r.elapsed_s << "s\n";
    }
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
        printLatStats(r.totalLat, "端到端 (含等锁)   ");
        printLatStats(r.waitLat, "等锁时间          ");
        printLatStats(r.processLat, "锁内处理时间      ");
    } else {
        printLatStats(r.totalLat, "端到端 (提交→回调)");
        printLatStats(r.submitLat, "提交延时 (入队)   ");
    }
    std::cout
        << "================================================================\n";
}

static void printSummaryTable(const std::vector<RoundResult> &results,
                              const std::string &title) {
    if (results.empty())
        return;
    bool isQueue = (results[0].mode == "queue");

    std::cout << "\n\n╔════════════════════════════════════════════════════════"
                 "═══════╗\n";
    std::cout << "║  " << std::left << std::setw(61) << title << "║\n";
    std::cout << "╠════════╦═══════════╦══════════╦══════════╦══════════╦══════"
                 "═══╣\n";

    if (isQueue) {
        // queue 模式：端到端 + 入队延时
        std::cout << "║ Threads║ Throughput║ E2E-p50  ║ E2E-p99  ║ Enq-p50  "
                     "║Enq-p99║\n";
        std::cout << "║ (+1 C) ║  (ops/s)  ║   (us)   ║   (us)   ║   (us)   ║  "
                     "(us) ║\n";
    } else {
        // mutex 模式：端到端 + 等锁延时
        std::cout << "║ Threads║ Throughput║ Lat-p50  ║ Lat-p99  ║ Wait-p50 "
                     "║Wait-p99║\n";
        std::cout << "║        ║  (ops/s)  ║   (us)   ║   (us)   ║   (us)   ║  "
                     "(us)  ║\n";
    }
    std::cout << "╠════════╬═══════════╬══════════╬══════════╬══════════╬══════"
                 "═══╣\n";

    for (auto &r : results) {
        std::cout << std::fixed;
        if (isQueue) {
            std::cout << "║ " << std::setw(6) << r.numThreads << " ║ "
                      << std::setw(9) << std::setprecision(0)
                      << r.aggregateThroughput << " ║ " << std::setw(8)
                      << std::setprecision(2) << r.totalLat.p50 << " ║ "
                      << std::setw(8) << std::setprecision(2) << r.totalLat.p99
                      << " ║ " << std::setw(8) << std::setprecision(2)
                      << r.submitLat.p50 << " ║ " << std::setw(7)
                      << std::setprecision(2) << r.submitLat.p99 << " ║\n";
        } else {
            std::cout << "║ " << std::setw(6) << r.numThreads << " ║ "
                      << std::setw(9) << std::setprecision(0)
                      << r.aggregateThroughput << " ║ " << std::setw(8)
                      << std::setprecision(2) << r.totalLat.p50 << " ║ "
                      << std::setw(8) << std::setprecision(2) << r.totalLat.p99
                      << " ║ " << std::setw(8) << std::setprecision(2)
                      << r.waitLat.p50 << " ║ " << std::setw(7)
                      << std::setprecision(2) << r.waitLat.p99 << " ║\n";
        }
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
            std::cout << "\n>>> [queue] " << nThreads
                      << " 生产者线程 (+1 consumer)..." << std::flush;
            auto r = runRoundQueue(cfg, nThreads);
            queueResults.push_back(r);
            printRound(r);
        }
        printSummaryTable(queueResults,
                          "MPSC Queue 模式 — 并发扩展性 (N+1线程)");
    }

    // 对比总结
    if (runMutex && runQueue && !mutexResults.empty() &&
        !queueResults.empty()) {
        std::cout
            << "\n\n━━━ Mutex vs Queue 对比 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        std::cout << "  注: mutex N线程 vs queue N生产者+1消费者\n\n";
        std::cout << std::fixed << std::setprecision(0);
        for (size_t i = 0; i < mutexResults.size() && i < queueResults.size();
             ++i) {
            auto &m = mutexResults[i];
            auto &q = queueResults[i];
            double speedup = q.aggregateThroughput / m.aggregateThroughput;
            std::cout << "  " << m.numThreads << " 线程: "
                      << "mutex=" << m.aggregateThroughput << " ops/s"
                      << " (e2e-p50=" << std::setprecision(1) << m.totalLat.p50
                      << "us), " << std::setprecision(0)
                      << "queue=" << q.aggregateThroughput << " ops/s"
                      << " (e2e-p50=" << std::setprecision(1) << q.totalLat.p50
                      << "us), " << std::setprecision(2) << speedup << "x\n"
                      << std::setprecision(0);
        }
    }

    return 0;
}
