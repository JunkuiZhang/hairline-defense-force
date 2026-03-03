/**
 * @file benchmark.cpp
 * @brief 纯撮合模式性能基线测试
 *
 * 在内存中预生成大批量订单 JSON，依次喂入 TradeSystem（纯撮合模式），
 * 精确计时每笔 handleOrder 的端到端延时，最终输出：
 *   - 吞吐量 (orders/s)
 *   - 延时分位数 p50 / p95 / p99 / max
 *
 * 用法:
 *   cmake --build build --target benchmark
 *   ./bin/benchmark [--orders N] [--securities M] [--shareholders K]
 * [--logging]
 */

#include "trade_system.h"
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

// --- 命令行参数 ---
struct Config {
    int numOrders = 100000;
    int numSecurities = 10;
    int numShareholders = 500;
    bool enableLogging = false;
    int warmupOrders = 1000;
};

static Config parseArgs(int argc, char *argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--orders" && i + 1 < argc)
            cfg.numOrders = std::atoi(argv[++i]);
        else if (arg == "--securities" && i + 1 < argc)
            cfg.numSecurities = std::atoi(argv[++i]);
        else if (arg == "--shareholders" && i + 1 < argc)
            cfg.numShareholders = std::atoi(argv[++i]);
        else if (arg == "--logging")
            cfg.enableLogging = true;
        else if (arg == "--warmup" && i + 1 < argc)
            cfg.warmupOrders = std::atoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") {
            std::cout << "用法: benchmark [选项]\n"
                      << "  --orders N         总订单量 (默认 100000)\n"
                      << "  --securities M     证券种类数 (默认 10)\n"
                      << "  --shareholders K   股东数 (默认 500)\n"
                      << "  --warmup W         热身订单数 (默认 1000)\n"
                      << "  --logging          启用日志写入\n";
            std::exit(0);
        }
    }
    return cfg;
}

// --- 订单生成器 ---
static std::vector<nlohmann::json> generateOrders(const Config &cfg, int count,
                                                  int idOffset = 0) {
    std::mt19937 rng(42 + idOffset);
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
        bool isBuy = sideDist(rng) == 0;
        int rawQty = qtyDist(rng) * 100;
        double rawPrice = priceDist(rng);
        double price = std::round(rawPrice * 100.0) / 100.0;

        nlohmann::json j;
        j["clOrderId"] = "B" + std::to_string(idOffset + i);
        j["market"] = "XSHG";
        j["securityId"] = securities[secDist(rng)];
        j["side"] = isBuy ? "B" : "S";
        j["price"] = price;
        j["qty"] = rawQty;
        j["shareholderId"] = shareholders[shDist(rng)];

        orders.push_back(std::move(j));
    }
    return orders;
}

// --- 统计工具 ---
struct Stats {
    double throughput;
    double mean_us;
    double p50_us;
    double p95_us;
    double p99_us;
    double max_us;
    size_t totalOrders;
    size_t execReports;   // 成交回报条数（每笔撮合产生 2 条：主动方+被动方）
    size_t rejectReports; // 风控拒绝回报条数
};

static Stats computeStats(std::vector<double> &latencies_us, double elapsed_s,
                          size_t matched, size_t rejected) {
    Stats s{};
    s.totalOrders = latencies_us.size();
    s.execReports = matched;
    s.rejectReports = rejected;
    s.throughput = s.totalOrders / elapsed_s;

    std::sort(latencies_us.begin(), latencies_us.end());
    double sum = std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0);
    s.mean_us = sum / s.totalOrders;

    auto pct = [&](double p) -> double {
        size_t idx = static_cast<size_t>(p / 100.0 * (s.totalOrders - 1));
        return latencies_us[idx];
    };

    s.p50_us = pct(50);
    s.p95_us = pct(95);
    s.p99_us = pct(99);
    s.max_us = latencies_us.back();
    return s;
}

static void printStats(const Stats &s, const std::string &label) {
    std::cout << "\n======================================================\n";
    std::cout << "  " << label << "\n";
    std::cout << "======================================================\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  总订单数:          " << s.totalOrders << "\n";
    std::cout << "  成交回报数:        " << s.execReports << "\n";
    std::cout << "  拒绝回报数:        " << s.rejectReports << "\n";
    std::cout << "------------------------------------------------------\n";
    std::cout << "  吞吐量:            " << s.throughput << " orders/s\n";
    std::cout << "------------------------------------------------------\n";
    std::cout << "  延时 mean:         " << s.mean_us << " us\n";
    std::cout << "  延时 p50:          " << s.p50_us << " us\n";
    std::cout << "  延时 p95:          " << s.p95_us << " us\n";
    std::cout << "  延时 p99:          " << s.p99_us << " us\n";
    std::cout << "  延时 max:          " << s.max_us << " us\n";
    std::cout << "======================================================\n";
}

// --- 主测试 ---
int main(int argc, char *argv[]) {
    Config cfg = parseArgs(argc, argv);

    std::cout << "=== 纯撮合模式 Benchmark ===\n"
              << "  订单数: " << cfg.numOrders
              << "  证券数: " << cfg.numSecurities
              << "  股东数: " << cfg.numShareholders
              << "  日志: " << (cfg.enableLogging ? "ON" : "OFF")
              << "  热身: " << cfg.warmupOrders << "\n\n";

    // 1. 预生成所有订单 JSON（不计入测时）
    std::cout << "正在预生成订单..." << std::flush;
    auto warmupOrders = generateOrders(cfg, cfg.warmupOrders, 0);
    auto testOrders = generateOrders(cfg, cfg.numOrders, cfg.warmupOrders);
    std::cout << " 完成 (" << warmupOrders.size() + testOrders.size()
              << " 笔)\n";

    // 2. 初始化交易系统（纯撮合模式）
    hdf::TradeSystem system;
    size_t matchedCount = 0;
    size_t rejectedCount = 0;

    system.setSendToClient([&](const nlohmann::json &resp) {
        if (resp.contains("execId"))
            ++matchedCount;
        else if (resp.contains("rejectCode"))
            ++rejectedCount;
    });

    if (cfg.enableLogging) {
        system.enableLogging("/tmp/benchmark_history.jsonl");
    }

    // 3. 热身
    std::cout << "热身中..." << std::flush;
    for (auto &order : warmupOrders) {
        system.handleOrder(order);
    }
    std::cout << " 完成\n";

    matchedCount = 0;
    rejectedCount = 0;

    // 4. 正式测量
    std::cout << "正式测量中..." << std::flush;
    std::vector<double> latencies_us;
    latencies_us.reserve(cfg.numOrders);

    auto wallStart = Clock::now();

    for (auto &order : testOrders) {
        auto t0 = Clock::now();
        system.handleOrder(order);
        auto t1 = Clock::now();

        double us = duration_cast<nanoseconds>(t1 - t0).count() / 1000.0;
        latencies_us.push_back(us);
    }

    auto wallEnd = Clock::now();
    double elapsed_s =
        duration_cast<microseconds>(wallEnd - wallStart).count() / 1e6;
    std::cout << " 完成 (" << elapsed_s << "s)\n";

    // 5. 输出结果
    auto stats =
        computeStats(latencies_us, elapsed_s, matchedCount, rejectedCount);
    printStats(stats, cfg.enableLogging ? "纯撮合 + 日志" : "纯撮合 (无日志)");

    // 6. 延时直方图
    std::cout << "\n延时分布 (us):\n";
    double bucketWidth = stats.p99_us / 10.0;
    if (bucketWidth < 0.01)
        bucketWidth = 1.0;
    std::vector<size_t> histogram(12, 0);
    for (double lat : latencies_us) {
        int bucket = static_cast<int>(lat / bucketWidth);
        if (bucket >= (int)histogram.size())
            bucket = histogram.size() - 1;
        histogram[bucket]++;
    }
    size_t maxBucket = *std::max_element(histogram.begin(), histogram.end());
    for (int i = 0; i < (int)histogram.size(); ++i) {
        double lo = i * bucketWidth;
        double hi = (i + 1) * bucketWidth;
        int barLen = maxBucket > 0 ? (int)(50.0 * histogram[i] / maxBucket) : 0;
        std::cout << std::fixed << std::setprecision(1) << "  [" << std::setw(8)
                  << lo << " - " << std::setw(8) << hi << ") " << std::setw(7)
                  << histogram[i] << " " << std::string(barLen, '#') << "\n";
    }

    if (cfg.enableLogging) {
        system.disableLogging();
    }

    return 0;
}
