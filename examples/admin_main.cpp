/**
 * @file admin_main.cpp
 * @brief 管理界面入口程序 — 交易所前置模式
 *
 * 架构：
 *   Admin UI (Python) → AdminServer (TCP) → gateway (前置系统) → exchange
 * (纯撮合，模拟交易所) ↓ 回报 AdminServer.broadcast → Python
 *
 * 数据流：
 *   1. Python 发送订单/撤单 → AdminServer 收到 →
 * gateway.handleOrder/handleCancel
 *   2. gateway 内部撮合 + 风控 → sendToExchange_ →
 * exchange.handleOrder/handleCancel
 *   3. exchange 撮合 → sendToClient_ → gateway.handleResponse
 *   4. gateway 处理回报 → sendToClient_ → AdminServer.broadcast → Python
 *
 * 用法：
 *   ./bin/admin_main              # 默认端口 9900
 *   ./bin/admin_main 9901         # 自定义端口
 *
 * 然后启动 Python 端：
 *   cd admin && bash start.sh
 */

#include "admin_server.h"
#include "trade_system.h"
#include <csignal>
#include <iostream>
#include <mutex>

static std::atomic<bool> g_running{true};

static void signalHandler(int /*sig*/) { g_running = false; }

int main(int argc, char *argv[]) {
    // 解析端口
    uint16_t port = 9900;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    // ---- 构建核心组件 ----
    hdf::TradeSystem gateway;  // 交易所前置系统
    hdf::TradeSystem exchange; // 纯撮合系统（模拟交易所）
    hdf::AdminServer adminServer(port);

    // 互斥锁：TradeSystem 非线程安全，所有操作需串行化
    std::mutex tradeMutex;

    // ---- 连接 gateway → Admin UI（回报广播） ----
    gateway.setSendToClient([&adminServer](const nlohmann::json &output) {
        std::cout << "[→Client] " << output.dump() << std::endl;
        adminServer.broadcast(output);
    });

    // ---- 连接 gateway → exchange（前置转发给交易所） ----
    // 根据是否含 origClOrderId 区分订单和撤单
    gateway.setSendToExchange(
        [&exchange, &tradeMutex](const nlohmann::json &req) {
            // 注意：此回调已在 tradeMutex 保护下被调用（从
            // handleOrder/handleCancel 内部触发） 但 exchange
            // 是独立实例，直接调用即可（同一线程，无需再加锁）
            std::cout << "[→Exchange] " << req.dump() << std::endl;
            if (req.contains("origClOrderId")) {
                exchange.handleCancel(req);
            } else {
                exchange.handleOrder(req);
            }
        });

    // ---- 连接 exchange → gateway（交易所回报返回前置） ----
    exchange.setSendToClient([&gateway](const nlohmann::json &resp) {
        std::cout << "[←Exchange] " << resp.dump() << std::endl;
        gateway.handleResponse(resp);
    });
    // exchange 不设置 sendToExchange_ → 纯撮合模式

    // ---- 连接 AdminServer → gateway（管理界面指令转发） ----
    adminServer.setOnOrder(
        [&gateway, &tradeMutex](const nlohmann::json &orderJson) {
            std::lock_guard<std::mutex> lock(tradeMutex);
            std::cout << "[Admin←] Order: " << orderJson.dump() << std::endl;
            gateway.handleOrder(orderJson);
        });

    adminServer.setOnCancel(
        [&gateway, &tradeMutex](const nlohmann::json &cancelJson) {
            std::lock_guard<std::mutex> lock(tradeMutex);
            std::cout << "[Admin←] Cancel: " << cancelJson.dump() << std::endl;
            gateway.handleCancel(cancelJson);
        });

    adminServer.setOnQuery([](const std::string &queryType) -> nlohmann::json {
        // TODO: 接入 MatchingEngine 的订单簿快照接口
        nlohmann::json result;
        result["queryType"] = queryType;
        if (queryType == "orderbook") {
            result["bids"] = nlohmann::json::array();
            result["asks"] = nlohmann::json::array();
            result["message"] = "Orderbook snapshot not yet implemented";
        } else if (queryType == "stats") {
            result["message"] = "Stats query not yet implemented";
        } else {
            result["message"] = "Unknown query type: " + queryType;
        }
        return result;
    });

    // ---- 启动 ----
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    adminServer.start();
    std::cout << "=== 交易所前置系统 Admin Server ===" << std::endl;
    std::cout << "=== Listening on port " << port << " ===" << std::endl;
    std::cout << "架构: Admin UI → gateway(前置) → exchange(模拟交易所)"
              << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;

    // 主线程阻塞等待退出信号
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "\nShutting down..." << std::endl;
    adminServer.stop();
    std::cout << "Done." << std::endl;

    return 0;
}
