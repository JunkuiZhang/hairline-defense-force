/**
 * @file bench_network.cpp
 * @brief 网络 I/O 层压测 —— 测量 AdminServer 的 TCP 收发性能
 *
 * 测量维度：
 *   1. 连接建立速率 (connections/s)
 *   2. 单连接消息吞吐量：单个 TCP 连接发送 N 条 JSON Lines，测 send → recv 延时
 *   3. 多连接并发吞吐量：M 个客户端各发 N/M 条消息，测聚合吞吐
 *   4. broadcast 吞吐量：1 条订单触发回报 → 广播到 K 个客户端的延时
 *
 * 架构：
 *   本进程内部启动 AdminServer（TCP），再起多个 client 线程用 socket 连接。
 *   AdminServer.setOnOrder 回调立即 broadcast 一条回报，客户端测量 send→recv
 * RTT。
 *
 * 用于对比：
 *   - 当前 thread-per-client + blocking I/O
 *   - 未来 epoll / io_uring 改造后
 *
 * 用法:
 *   ./bin/bench_network                          # 默认参数
 *   ./bin/bench_network --clients 1,4,8,16,32    # 测指定客户端数
 *   ./bin/bench_network --messages 50000          # 每轮总消息数
 *   ./bin/bench_network --broadcast-clients 50    # broadcast 压测最大客户端数
 */

#include "admin_server.h"
#include <algorithm>
#include <arpa/inet.h>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <numeric>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::high_resolution_clock;
using namespace std::chrono;

// ── 配置 ────────────────────────────────────────────────────
struct Config {
    int totalMessages = 20000; // 每轮总消息数
    std::vector<int> clientCounts = {1,  2,  4,  8,
                                     16, 32, 64, 128}; // 客户端数列表
    int warmupMessages = 500;                          // 热身消息数
    uint16_t port = 19900;        // 测试用端口(避免与生产冲突)
    int broadcastMaxClients = 32; // broadcast 压测最大客户端数
    bool runRtt = true;           // 是否跑 RTT 测试
    bool runBroadcast = true;     // 是否跑 broadcast 测试
    bool runConnect = true;       // 是否跑连接建立测试
    int connectIterations = 500;  // 连接建立测试次数
};

static Config parseArgs(int argc, char *argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--messages" && i + 1 < argc)
            cfg.totalMessages = std::atoi(argv[++i]);
        else if (a == "--clients" && i + 1 < argc) {
            cfg.clientCounts.clear();
            std::string list = argv[++i];
            std::istringstream ss(list);
            std::string token;
            while (std::getline(ss, token, ','))
                cfg.clientCounts.push_back(std::atoi(token.c_str()));
        } else if (a == "--port" && i + 1 < argc)
            cfg.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--broadcast-clients" && i + 1 < argc)
            cfg.broadcastMaxClients = std::atoi(argv[++i]);
        else if (a == "--connect-iters" && i + 1 < argc)
            cfg.connectIterations = std::atoi(argv[++i]);
        else if (a == "--warmup" && i + 1 < argc)
            cfg.warmupMessages = std::atoi(argv[++i]);
        else if (a == "--no-rtt")
            cfg.runRtt = false;
        else if (a == "--no-broadcast")
            cfg.runBroadcast = false;
        else if (a == "--no-connect")
            cfg.runConnect = false;
        else if (a == "--help" || a == "-h") {
            std::cout
                << "用法: bench_network [选项]\n"
                << "  --messages N          每轮总消息数 (默认 20000)\n"
                << "  --clients C           客户端数列表，逗号分隔 (默认 "
                   "1,2,4,8,16)\n"
                << "  --port P              测试端口 (默认 19900)\n"
                << "  --broadcast-clients K broadcast 最大客户端数 (默认 32)\n"
                << "  --connect-iters N     连接建立测试次数 (默认 500)\n"
                << "  --warmup W            热身消息数 (默认 500)\n"
                << "  --no-rtt              跳过 RTT 测试\n"
                << "  --no-broadcast        跳过 broadcast 测试\n"
                << "  --no-connect          跳过连接建立测试\n";
            std::exit(0);
        }
    }
    return cfg;
}

// ── TCP 客户端工具 ──────────────────────────────────────────
static int connectToServer(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    // TCP_NODELAY — 减少小包延迟
    int flag = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// 发送一条 JSON Line（带 '\n'）
static bool sendJsonLine(int fd, const std::string &json) {
    std::string data = json + "\n";
    ssize_t remaining = static_cast<ssize_t>(data.size());
    ssize_t totalSent = 0;
    while (remaining > 0) {
        ssize_t sent = ::send(fd, data.c_str() + totalSent,
                              static_cast<size_t>(remaining), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        totalSent += sent;
        remaining -= sent;
    }
    return true;
}

// 读取一行（阻塞，直到收到 '\n'）
static std::string recvLine(int fd) {
    std::string result;
    char c;
    while (true) {
        ssize_t n = ::recv(fd, &c, 1, 0);
        if (n <= 0)
            return ""; // EOF 或错误
        if (c == '\n')
            return result;
        result += c;
    }
}

// 读取一行（非阻塞尝试，用 recv + MSG_DONTWAIT），收不到返回空
// 用于 broadcast 测试中的快速轮询
static std::string tryRecvLine(int fd, int timeoutMs) {
    // 先用 poll/select 等可读
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int ret = ::select(fd + 1, &readfds, nullptr, nullptr, &tv);
    if (ret <= 0)
        return "";
    return recvLine(fd);
}

// ── 百分位计算 ──────────────────────────────────────────
static double percentile(std::vector<double> &sorted, double pct) {
    if (sorted.empty())
        return 0;
    size_t idx = static_cast<size_t>(pct / 100.0 * (sorted.size() - 1));
    return sorted[std::min(idx, sorted.size() - 1)];
}

// ── 生成订单 JSON ──────────────────────────────────────────
static std::string makeOrderJson(int seq) {
    // 极简 JSON 订单，用于网络层压测
    std::string json =
        R"({"type":"order","clOrderId":"BENCH-)" + std::to_string(seq) +
        R"(","market":"SZ","securityId":"000001","side":"buy","price":10.0,"qty":100,"shareholderId":"SH001"})";
    return json;
}

static std::string makeQueryJson() {
    return R"({"type":"query","queryType":"orderbook"})";
}

// ══════════════════════════════════════════════════════════════
// 1. 连接建立速率测试
// ══════════════════════════════════════════════════════════════
struct ConnectResult {
    int iterations;
    double totalTimeMs;
    double connPerSec;
    double p50Us, p95Us, p99Us, maxUs;
};

static ConnectResult benchConnect(uint16_t port, int iterations) {
    std::vector<double> latencies;
    latencies.reserve(iterations);

    auto totalStart = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto t0 = Clock::now();
        int fd = connectToServer(port);
        auto t1 = Clock::now();
        if (fd < 0)
            continue;

        double us = duration_cast<nanoseconds>(t1 - t0).count() / 1000.0;
        latencies.push_back(us);
        ::close(fd);
    }
    auto totalEnd = Clock::now();
    double totalMs =
        duration_cast<microseconds>(totalEnd - totalStart).count() / 1000.0;

    std::sort(latencies.begin(), latencies.end());

    ConnectResult r;
    r.iterations = static_cast<int>(latencies.size());
    r.totalTimeMs = totalMs;
    r.connPerSec = latencies.size() / (totalMs / 1000.0);
    r.p50Us = percentile(latencies, 50);
    r.p95Us = percentile(latencies, 95);
    r.p99Us = percentile(latencies, 99);
    r.maxUs = latencies.empty() ? 0 : latencies.back();
    return r;
}

// ══════════════════════════════════════════════════════════════
// 2. 消息 RTT 测试（send order → recv broadcast response）
// ══════════════════════════════════════════════════════════════
struct RttResult {
    int numClients;
    int totalMessages;
    double totalTimeSec;
    double throughputOpsPerSec; // 聚合消息吞吐
    // RTT 延时 (us) — 从 send 到收到回报
    double rttP50, rttP95, rttP99, rttMax, rttMean;
};

static RttResult benchRtt(uint16_t port, int numClients, int totalMessages,
                          int warmupMessages) {
    // 连接所有客户端
    std::vector<int> clientFds(numClients);
    for (int i = 0; i < numClients; ++i) {
        clientFds[i] = connectToServer(port);
        if (clientFds[i] < 0) {
            std::cerr << "[benchRtt] 连接失败，客户端 " << i << std::endl;
            // 清理已连接的
            for (int j = 0; j < i; ++j)
                ::close(clientFds[j]);
            return {};
        }
    }

    // 给服务端一点时间注册所有客户端
    std::this_thread::sleep_for(milliseconds(50));

    int messagesPerClient = totalMessages / numClients;
    int warmupPer = warmupMessages / numClients;

    // 结果收集
    std::mutex resultsMutex;
    std::vector<double> allRtts;
    allRtts.reserve(totalMessages);

    // 同步屏障 — 所有线程同时开始
    std::barrier startBarrier(numClients + 1);

    // 每个客户端一个线程
    std::vector<std::thread> threads;
    for (int c = 0; c < numClients; ++c) {
        threads.emplace_back([&, c, messagesPerClient, warmupPer]() {
            int fd = clientFds[c];
            std::vector<double> localRtts;
            localRtts.reserve(messagesPerClient);

            startBarrier.arrive_and_wait();

            int total = warmupPer + messagesPerClient;
            for (int i = 0; i < total; ++i) {
                int seq = c * total + i;
                std::string json = makeOrderJson(seq);

                auto t0 = Clock::now();
                if (!sendJsonLine(fd, json))
                    break;

                // 等待回报（server broadcast 回来）
                // 多客户端场景下，每条 order 会 broadcast 给所有客户端
                // 当前客户端只关心收到一条 response
                std::string resp = recvLine(fd);
                auto t1 = Clock::now();

                if (resp.empty())
                    break;

                // 跳过热身期
                if (i >= warmupPer) {
                    double us =
                        duration_cast<nanoseconds>(t1 - t0).count() / 1000.0;
                    localRtts.push_back(us);
                }

                // 多客户端场景：可能收到来自其他客户端触发的 broadcast
                // 这里简化处理 — 只取第一条 response 作为 RTT 测量
                // 排空可能的额外 broadcast 消息
                if (numClients > 1) {
                    for (int drain = 0; drain < numClients - 1; ++drain) {
                        std::string extra = tryRecvLine(fd, 5);
                        if (extra.empty())
                            break;
                    }
                }
            }

            std::lock_guard<std::mutex> lock(resultsMutex);
            allRtts.insert(allRtts.end(), localRtts.begin(), localRtts.end());
        });
    }

    // 统一起跑
    auto wallStart = Clock::now();
    startBarrier.arrive_and_wait();

    for (auto &t : threads)
        t.join();
    auto wallEnd = Clock::now();

    // 关闭客户端
    for (int fd : clientFds)
        ::close(fd);

    // 统计
    std::sort(allRtts.begin(), allRtts.end());
    double totalSec =
        duration_cast<microseconds>(wallEnd - wallStart).count() / 1e6;
    double mean = allRtts.empty()
                      ? 0
                      : std::accumulate(allRtts.begin(), allRtts.end(), 0.0) /
                            allRtts.size();

    RttResult r;
    r.numClients = numClients;
    r.totalMessages = static_cast<int>(allRtts.size());
    r.totalTimeSec = totalSec;
    r.throughputOpsPerSec = allRtts.size() / totalSec;
    r.rttMean = mean;
    r.rttP50 = percentile(allRtts, 50);
    r.rttP95 = percentile(allRtts, 95);
    r.rttP99 = percentile(allRtts, 99);
    r.rttMax = allRtts.empty() ? 0 : allRtts.back();
    return r;
}

// ══════════════════════════════════════════════════════════════
// 3. Broadcast 扇出测试
//    单个生产者发 1 条消息，N 个被动客户端测量接收延时
// ══════════════════════════════════════════════════════════════
struct BroadcastResult {
    int numReceivers;
    int totalBroadcasts;
    double broadcastP50, broadcastP95, broadcastP99, broadcastMax;
    double receiverP50, receiverP95, receiverP99, receiverMax;
    double throughput; // broadcasts/sec
};

static BroadcastResult benchBroadcast(uint16_t port, int numReceivers,
                                      int totalBroadcasts) {
    // 1 个 producer + N 个 receiver
    int producerFd = connectToServer(port);
    if (producerFd < 0) {
        std::cerr << "[benchBroadcast] producer 连接失败\n";
        return {};
    }

    std::vector<int> receiverFds(numReceivers);
    for (int i = 0; i < numReceivers; ++i) {
        receiverFds[i] = connectToServer(port);
        if (receiverFds[i] < 0) {
            std::cerr << "[benchBroadcast] receiver " << i << " 连接失败\n";
            for (int j = 0; j < i; ++j)
                ::close(receiverFds[j]);
            ::close(producerFd);
            return {};
        }
    }

    std::this_thread::sleep_for(milliseconds(50));

    // receivers 线程：等待接收广播
    std::mutex recvMutex;
    std::vector<double> receiverLatencies;
    receiverLatencies.reserve(totalBroadcasts * numReceivers);

    std::atomic<int> receivedCount{0};
    std::atomic<bool> done{false};

    std::vector<std::thread> recvThreads;
    for (int r = 0; r < numReceivers; ++r) {
        recvThreads.emplace_back([&, r]() {
            int fd = receiverFds[r];
            while (!done.load(std::memory_order_relaxed)) {
                std::string line = tryRecvLine(fd, 100);
                if (line.empty())
                    continue;

                // 每收到一条就 count++
                receivedCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // producer 也需要排空自己收到的 broadcast
    std::thread producerDrain([&]() {
        while (!done.load(std::memory_order_relaxed)) {
            tryRecvLine(producerFd, 100);
        }
    });

    // Producer 发 N 条订单，测量每条从 send 到所有 receiver 收到的时间
    std::vector<double> broadcastLatencies;
    broadcastLatencies.reserve(totalBroadcasts);

    // 热身
    int warmup = std::min(50, totalBroadcasts / 10);
    for (int i = 0; i < warmup; ++i) {
        sendJsonLine(producerFd, makeOrderJson(i));
        std::this_thread::sleep_for(microseconds(200));
    }
    // 等排空
    std::this_thread::sleep_for(milliseconds(100));
    receivedCount.store(0);

    auto totalStart = Clock::now();

    for (int i = 0; i < totalBroadcasts; ++i) {
        auto t0 = Clock::now();
        sendJsonLine(producerFd, makeOrderJson(warmup + i));

        // 等待所有 receiver + producer 自身 都收到（共 numReceivers+1
        // 个客户端） 但 producer drain 线程在单独排空，这里只等 receiver
        int expected = (i + 1) * numReceivers;
        int spins = 0;
        while (receivedCount.load(std::memory_order_relaxed) < expected) {
            if (++spins > 5000000)
                break; // 防死循环 ~5s
            std::this_thread::yield();
        }
        auto t1 = Clock::now();
        double us = duration_cast<nanoseconds>(t1 - t0).count() / 1000.0;
        broadcastLatencies.push_back(us);
    }

    auto totalEnd = Clock::now();

    done.store(true);
    for (auto &t : recvThreads)
        t.join();
    producerDrain.join();

    // 清理
    ::close(producerFd);
    for (int fd : receiverFds)
        ::close(fd);

    // 统计
    std::sort(broadcastLatencies.begin(), broadcastLatencies.end());
    double totalSec =
        duration_cast<microseconds>(totalEnd - totalStart).count() / 1e6;

    BroadcastResult br;
    br.numReceivers = numReceivers;
    br.totalBroadcasts = totalBroadcasts;
    br.throughput = totalBroadcasts / totalSec;
    br.broadcastP50 = percentile(broadcastLatencies, 50);
    br.broadcastP95 = percentile(broadcastLatencies, 95);
    br.broadcastP99 = percentile(broadcastLatencies, 99);
    br.broadcastMax =
        broadcastLatencies.empty() ? 0 : broadcastLatencies.back();

    // receiver 粒度延时这里不单独测了（需要改 protocol），只看整体
    br.receiverP50 = br.receiverP95 = br.receiverP99 = br.receiverMax = 0;
    return br;
}

// ── 打印工具 ───────────────────────────────────────────────

static void printConnectResult(const ConnectResult &r) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout
        << "================================================================\n"
        << "  连接建立速率测试\n"
        << "  迭代次数: " << r.iterations << "    耗时: " << r.totalTimeMs
        << " ms\n"
        << "----------------------------------------------------------------\n"
        << "  连接速率: " << std::setprecision(0) << r.connPerSec << " conn/s\n"
        << std::setprecision(2) << "  延时 (us):  p50=" << r.p50Us
        << "  p95=" << r.p95Us << "  p99=" << r.p99Us << "  max=" << r.maxUs
        << "\n"
        << "================================================================"
           "\n\n";
}

static void printRttResult(const RttResult &r) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout
        << "================================================================\n"
        << "  [RTT] 客户端数: " << r.numClients
        << "    消息数: " << r.totalMessages
        << "    耗时: " << std::setprecision(3) << r.totalTimeSec << "s\n"
        << "----------------------------------------------------------------\n"
        << "  聚合吞吐量: " << std::setprecision(0) << r.throughputOpsPerSec
        << " msg/s\n"
        << std::setprecision(2) << "  RTT (us):  mean=" << r.rttMean
        << "  p50=" << r.rttP50 << "  p95=" << r.rttP95 << "  p99=" << r.rttP99
        << "  max=" << r.rttMax << "\n"
        << "================================================================"
           "\n\n";
}

static void printBroadcastResult(const BroadcastResult &r) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout
        << "================================================================\n"
        << "  [Broadcast] 接收者: " << r.numReceivers
        << "    广播次数: " << r.totalBroadcasts << "\n"
        << "----------------------------------------------------------------\n"
        << "  吞吐量: " << std::setprecision(0) << r.throughput
        << " broadcasts/s\n"
        << std::setprecision(2) << "  广播延时 (send → all recv, us):\n"
        << "    p50=" << r.broadcastP50 << "  p95=" << r.broadcastP95
        << "  p99=" << r.broadcastP99 << "  max=" << r.broadcastMax << "\n"
        << "================================================================"
           "\n\n";
}

// ══════════════════════════════════════════════════════════════
// main
// ══════════════════════════════════════════════════════════════
int main(int argc, char *argv[]) {
    Config cfg = parseArgs(argc, argv);

    std::cout << "=== 网络 I/O 层 Benchmark ===\n"
              << "  端口: " << cfg.port << "  消息数: " << cfg.totalMessages
              << "  热身: " << cfg.warmupMessages << "\n"
              << "  客户端数列表: ";
    for (size_t i = 0; i < cfg.clientCounts.size(); ++i) {
        if (i > 0)
            std::cout << ",";
        std::cout << cfg.clientCounts[i];
    }
    std::cout << "\n  硬件线程数: " << std::thread::hardware_concurrency()
              << "\n\n";

    // 启动 AdminServer —— 设置 echo 式回调
    // onOrder: 收到订单后立即 broadcast 一条确认回报（模拟最小业务开销）
    hdf::AdminServer server(cfg.port);
    server.setVerbose(false);

    // 用 atomic 计数器作为订单确认的 clOrderId
    std::atomic<int> orderCount{0};

    server.setOnOrder([&server, &orderCount](const nlohmann::json &orderJson) {
        // 生成最小回报 broadcast
        int seq = orderCount.fetch_add(1, std::memory_order_relaxed);
        nlohmann::json resp;
        resp["type"] = "response";
        resp["msgType"] = "confirm";
        resp["clOrderId"] = orderJson.value("clOrderId", "");
        resp["seq"] = seq;
        server.broadcast(resp);
    });

    server.setOnCancel([](const nlohmann::json &) {});
    server.setOnQuery([](const nlohmann::json &) -> nlohmann::json {
        return {{"type", "snapshot"}, {"data", "empty"}};
    });

    server.start();
    // 等待 server 完全启动
    std::this_thread::sleep_for(milliseconds(100));

    // ━━━ 1. 连接建立速率 ━━━
    if (cfg.runConnect) {
        std::cout << "━━━ 1. 连接建立速率 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
        auto cr = benchConnect(cfg.port, cfg.connectIterations);
        printConnectResult(cr);
    }

    // ━━━ 2. 消息 RTT (send→recv) ━━━
    if (cfg.runRtt) {
        std::cout
            << "━━━ 2. 消息 RTT (send order → recv broadcast) ━━━━━━━\n\n";

        std::vector<RttResult> rttResults;
        for (int nc : cfg.clientCounts) {
            std::cout << ">>> " << nc << " 客户端...\n";
            orderCount.store(0);
            auto r =
                benchRtt(cfg.port, nc, cfg.totalMessages, cfg.warmupMessages);
            rttResults.push_back(r);
            printRttResult(r);
            // 间隔让 server 线程清理
            std::this_thread::sleep_for(milliseconds(200));
        }

        // 汇总表
        std::cout << "╔════════════════════════════════════════════════════════"
                     "═══════╗\n"
                  << "║  消息 RTT — 并发扩展性                                 "
                     "     ║\n"
                  << "╠═════════╦════════════╦══════════╦══════════╦═══════════"
                     "═══════╣\n"
                  << "║ Clients ║ Throughput ║ RTT-p50  ║ RTT-p99  ║ RTT-max   "
                     "       ║\n"
                  << "║         ║  (msg/s)   ║   (us)   ║   (us)   ║   (us)    "
                     "       ║\n"
                  << "╠═════════╬════════════╬══════════╬══════════╬═══════════"
                     "═══════╣\n";
        for (auto &r : rttResults) {
            std::cout << std::fixed << "║ " << std::setw(7) << r.numClients
                      << " ║ " << std::setw(10) << std::setprecision(0)
                      << r.throughputOpsPerSec << " ║ " << std::setw(8)
                      << std::setprecision(2) << r.rttP50 << " ║ "
                      << std::setw(8) << r.rttP99 << " ║ " << std::setw(16)
                      << r.rttMax << " ║\n";
        }
        std::cout << "╚═════════╩════════════╩══════════╩══════════╩═══════════"
                     "═══════╝\n\n";

        if (rttResults.size() >= 2) {
            double base = rttResults[0].throughputOpsPerSec;
            std::cout << "  扩展效率 (相对 1 客户端):\n";
            for (size_t i = 1; i < rttResults.size(); ++i) {
                double ratio = rttResults[i].throughputOpsPerSec / base;
                std::cout << "    " << rttResults[i].numClients
                          << " 客户端: " << std::setprecision(2) << ratio
                          << "x 吞吐比\n";
            }
            std::cout << "\n";
        }
    }

    // ━━━ 3. Broadcast 扇出测试 ━━━
    if (cfg.runBroadcast) {
        std::cout << "━━━ 3. Broadcast 扇出延时 ━━━━━━━━━━━━━━━━━━━━━━━━\n\n";

        std::vector<BroadcastResult> bcResults;
        // 测不同接收者数量: 1, 4, 8, 16, 32, ...
        std::vector<int> receiverCounts;
        for (int n = 1; n <= cfg.broadcastMaxClients; n *= 2) {
            receiverCounts.push_back(n);
        }
        if (receiverCounts.back() != cfg.broadcastMaxClients) {
            receiverCounts.push_back(cfg.broadcastMaxClients);
        }

        int broadcastMessages = std::min(2000, cfg.totalMessages / 5);

        for (int nr : receiverCounts) {
            std::cout << ">>> " << nr << " 接收者...\n";
            orderCount.store(0);
            auto r = benchBroadcast(cfg.port, nr, broadcastMessages);
            bcResults.push_back(r);
            printBroadcastResult(r);
            std::this_thread::sleep_for(milliseconds(200));
        }

        // 汇总
        std::cout
            << "╔═══════════════════════════════════════════════════════════╗\n"
            << "║  Broadcast 扇出 — 接收者扩展性                           ║\n"
            << "╠═══════════╦════════════╦══════════╦══════════╦════════════╣\n"
            << "║ Receivers ║ Throughput ║ Lat-p50  ║ Lat-p99  ║ Lat-max    ║\n"
            << "║           ║ (bcast/s)  ║   (us)   ║   (us)   ║   (us)     ║\n"
            << "╠═══════════╬════════════╬══════════╬══════════╬════════════╣"
               "\n";
        for (auto &r : bcResults) {
            std::cout << std::fixed << "║ " << std::setw(9) << r.numReceivers
                      << " ║ " << std::setw(10) << std::setprecision(0)
                      << r.throughput << " ║ " << std::setw(8)
                      << std::setprecision(2) << r.broadcastP50 << " ║ "
                      << std::setw(8) << r.broadcastP99 << " ║ "
                      << std::setw(10) << r.broadcastMax << " ║\n";
        }
        std::cout << "╚═══════════╩════════════╩══════════╩══════════╩═════════"
                     "═══╝\n\n";
    }

    server.stop();
    std::cout << "=== 网络 I/O 层 Benchmark 完成 ===\n";

    return 0;
}
