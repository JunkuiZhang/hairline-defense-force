/**
 * @file bench_sharded.cpp
 * @brief 分片撮合引擎 vs 单线程基线 — 纯 CPU 处理吞吐对比
 *
 * 直接使用 Order 结构体（跳过 JSON 解析、风控、日志），
 * 专注衡量撮合引擎本身的并行扩展能力。
 *
 * 用法:
 *   ./bin/bench_sharded                        # 默认参数
 *   ./bin/bench_sharded --orders 200000 --securities 50 --shards 1,2,4,8
 */

#include "matching_engine.h"
#include "sharded_matching_engine.h"
#include "types.h"

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

using Clock = std::chrono::high_resolution_clock;
using namespace std::chrono;

// ── 配置 ────────────────────────────────────────────────────
struct Config {
    int totalOrders = 200000;
    int numSecurities = 20;
    int numShareholders = 500;
    int warmupOrders = 5000;
    std::vector<int> shardCounts = {1, 2, 4, 8};
};

static Config parseArgs(int argc, char *argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--orders" && i + 1 < argc)
            cfg.totalOrders = std::atoi(argv[++i]);
        else if (a == "--securities" && i + 1 < argc)
            cfg.numSecurities = std::atoi(argv[++i]);
        else if (a == "--shareholders" && i + 1 < argc)
            cfg.numShareholders = std::atoi(argv[++i]);
        else if (a == "--warmup" && i + 1 < argc)
            cfg.warmupOrders = std::atoi(argv[++i]);
        else if (a == "--shards" && i + 1 < argc) {
            cfg.shardCounts.clear();
            std::string list = argv[++i];
            std::istringstream ss(list);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                int n = std::atoi(tok.c_str());
                if (n > 0)
                    cfg.shardCounts.push_back(n);
            }
        } else if (a == "--help" || a == "-h") {
            std::cout
                << "用法: bench_sharded [选项]\n"
                << "  --orders N       总订单量 (默认 200000)\n"
                << "  --securities M   证券种类数 (默认 20)\n"
                << "  --shareholders K 股东数 (默认 500)\n"
                << "  --warmup W       热身订单数 (默认 5000)\n"
                << "  --shards S       分片数列表，逗号分隔 (默认 1,2,4,8)\n";
            std::exit(0);
        }
    }
    return cfg;
}

// ── 订单生成器（直接生成 Order 结构体，跳过 JSON）───────────
static std::vector<hdf::Order> generateOrders(const Config &cfg, int count,
                                              int idOffset, int seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> secDist(0, cfg.numSecurities - 1);
    std::uniform_int_distribution<int> shDist(0, cfg.numShareholders - 1);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_real_distribution<double> priceDist(5.0, 50.0);
    std::uniform_int_distribution<int> qtyDist(1, 100);

    // 预分配证券和股东列表
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
        o.clOrderId = "T" + std::to_string(idOffset + i);
        o.market = hdf::Market::XSHG;
        o.securityId = securities[secDist(rng)];
        o.side = sideDist(rng) == 0 ? hdf::Side::BUY : hdf::Side::SELL;
        o.price = std::round(priceDist(rng) * 100.0) / 100.0;
        o.qty = static_cast<uint32_t>(qtyDist(rng) * 100);
        o.shareholderId = shareholders[shDist(rng)];
        orders.push_back(std::move(o));
    }
    return orders;
}

// ── 延时统计 ─────────────────────────────────────────────────
struct LatStats {
    double mean = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
    size_t count = 0;
};
static LatStats calcStats(std::vector<double> &v) {
    LatStats s;
    s.count = v.size();
    if (s.count == 0)
        return s;
    std::sort(v.begin(), v.end());
    s.mean = std::accumulate(v.begin(), v.end(), 0.0) / s.count;
    auto p = [&](double pct) {
        return v[std::min(size_t(pct / 100.0 * (s.count - 1)), s.count - 1)];
    };
    s.p50 = p(50);
    s.p95 = p(95);
    s.p99 = p(99);
    s.max = v.back();
    return s;
}

// ── 单轮结果 ─────────────────────────────────────────────────
struct RoundResult {
    std::string label;
    int numShards;
    double elapsed_s;
    double throughput; // orders/s
    size_t execCount;
    LatStats submitLat; // 生产者提交延时（入队）
};

// ── 基线：单线程单 MatchingEngine，直接在调用线程内处理 ──────
static RoundResult runBaseline(const Config &cfg,
                               const std::vector<hdf::Order> &warmup,
                               const std::vector<hdf::Order> &orders) {
    hdf::MatchingEngine engine;
    size_t execCount = 0;

    // 热身
    for (const auto &o : warmup) {
        auto r = engine.match(o);
        execCount += r.executions.size();
        if (r.remainingQty > 0) {
            auto rem = o;
            rem.qty = r.remainingQty;
            engine.addOrder(rem);
        }
    }
    execCount = 0;

    // 计时
    auto t0 = Clock::now();
    for (const auto &o : orders) {
        auto r = engine.match(o);
        execCount += r.executions.size();
        if (r.remainingQty > 0) {
            auto rem = o;
            rem.qty = r.remainingQty;
            engine.addOrder(rem);
        }
    }
    auto t1 = Clock::now();

    double elapsed = duration_cast<microseconds>(t1 - t0).count() / 1e6;
    RoundResult r;
    r.label = "baseline (1 thread, 1 engine)";
    r.numShards = 1;
    r.elapsed_s = elapsed;
    r.throughput = orders.size() / elapsed;
    r.execCount = execCount;
    return r;
}

// ── 分片测试：1 生产者线程 + N 消费者线程 ────────────────────
// 生产者以最大速度提交所有订单，测量从第一条提交到最后一条处理完毕的
// 总 wall time，以及生产者侧的入队延时。
static RoundResult runSharded(const Config &cfg, int numShards,
                              const std::vector<hdf::Order> &warmup,
                              const std::vector<hdf::Order> &orders) {
    hdf::ShardedMatchingEngine engine(numShards);
    std::atomic<size_t> execCount{0};

    engine.setExecCallback(
        [&](const hdf::OrderResponse &, const std::string &) {
            execCount.fetch_add(1, std::memory_order_relaxed);
        });
    engine.start();

    // 热身（直接提交，不计时）
    for (const auto &o : warmup)
        engine.submitOrder(o);
    // 等热身排空
    while (engine.totalQueueDepth() > 0)
        std::this_thread::sleep_for(microseconds(200));
    std::this_thread::sleep_for(milliseconds(5));
    execCount = 0;

    // 测量提交延时
    std::vector<double> submitLats;
    submitLats.reserve(orders.size());

    auto wallStart = Clock::now();
    for (const auto &o : orders) {
        auto ts = Clock::now();
        engine.submitOrder(o);
        auto te = Clock::now();
        submitLats.push_back(duration_cast<nanoseconds>(te - ts).count() /
                             1000.0);
    }

    // stop() 等待所有分片队列排空并 join 工作线程，确保计时准确
    engine.stop();
    auto wallEnd = Clock::now();
    double elapsed =
        duration_cast<microseconds>(wallEnd - wallStart).count() / 1e6;

    RoundResult r;
    r.label = std::to_string(numShards) + " shards (" +
              std::to_string(numShards) + " consumer threads)";
    r.numShards = numShards;
    r.elapsed_s = elapsed;
    r.throughput = orders.size() / elapsed;
    r.execCount = execCount.load();
    r.submitLat = calcStats(submitLats);
    return r;
}

// ── 输出 ─────────────────────────────────────────────────────
static void printRound(const RoundResult &r, double baselineTput) {
    std::cout << std::fixed;
    std::cout << "\n  [" << r.label << "]\n";
    std::cout << "    耗时: " << std::setprecision(3) << r.elapsed_s
              << "s    成交回报: " << r.execCount << "\n";
    std::cout << "    吞吐量: " << std::setprecision(0) << r.throughput
              << " orders/s";
    if (baselineTput > 0 && r.label.find("baseline") == std::string::npos) {
        double speedup = r.throughput / baselineTput;
        double ideal = static_cast<double>(r.numShards);
        double eff = speedup / ideal * 100.0;
        std::cout << std::setprecision(2) << "   (" << speedup << "x, 效率 "
                  << eff << "%)";
    }
    std::cout << "\n";
    if (r.submitLat.count > 0) {
        std::cout << std::setprecision(2)
                  << "    入队延时: mean=" << r.submitLat.mean
                  << " p50=" << r.submitLat.p50 << " p99=" << r.submitLat.p99
                  << " max=" << r.submitLat.max << " us\n";
    }
}

static void printSummary(const std::vector<RoundResult> &all) {
    std::cout
        << "\n╔═══════════════════════════════════════════════════════╗\n";
    std::cout << "║  分片扩展性总结                                       ║\n";
    std::cout << "╠═══════╦═══════════╦══════════╦══════════╦═════════════╣\n";
    std::cout << "║ Shards║ Throughput║ Speedup  ║ Eff.(%)  ║ Enq-p99(us) ║\n";
    std::cout << "╠═══════╬═══════════╬══════════╬══════════╬═════════════╣\n";

    double baseline = all.empty() ? 1.0 : all[0].throughput;
    for (const auto &r : all) {
        double speedup = r.throughput / baseline;
        double eff = (r.numShards > 0) ? speedup / r.numShards * 100.0 : 100.0;
        std::cout << std::fixed << "║ " << std::setw(5) << r.numShards << " ║ "
                  << std::setw(9) << std::setprecision(0) << r.throughput
                  << " ║ " << std::setw(8) << std::setprecision(2) << speedup
                  << " ║ " << std::setw(8) << std::setprecision(1) << eff
                  << " ║ " << std::setw(11) << std::setprecision(2)
                  << r.submitLat.p99 << " ║\n";
    }
    std::cout << "╚═══════╩═══════════╩══════════╩══════════╩═════════════╝\n";
}

// ── main ─────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    Config cfg = parseArgs(argc, argv);

    unsigned int hw = std::thread::hardware_concurrency();
    std::cout << "=== 分片撮合引擎 Benchmark ===\n"
              << "  总订单: " << cfg.totalOrders
              << "  证券数: " << cfg.numSecurities
              << "  股东数: " << cfg.numShareholders
              << "  热身: " << cfg.warmupOrders << "  硬件线程: " << hw
              << "\n  测试分片数: ";
    for (size_t i = 0; i < cfg.shardCounts.size(); ++i) {
        if (i)
            std::cout << ",";
        std::cout << cfg.shardCounts[i];
    }
    std::cout << "\n\n";

    // 预生成所有订单（共享相同数据，保证各组测试可比）
    auto warmup = generateOrders(cfg, cfg.warmupOrders, 0, 9999);
    auto orders = generateOrders(cfg, cfg.totalOrders, cfg.warmupOrders, 42);

    std::vector<RoundResult> results;

    // 1. 单线程基线
    std::cout << ">>> 单线程基线..." << std::flush;
    auto base = runBaseline(cfg, warmup, orders);
    results.push_back(base);
    double baseTput = base.throughput;
    printRound(base, 0.0);

    // 2. 各分片数测试
    for (int ns : cfg.shardCounts) {
        std::cout << "\n>>> " << ns << " 分片..." << std::flush;
        auto r = runSharded(cfg, ns, warmup, orders);
        results.push_back(r);
        printRound(r, baseTput);
    }

    printSummary(results);
    return 0;
}
