/**
 * @file admin_main.cpp
 * @brief 管理界面入口程序
 *
 * 启动 TradeSystem（纯撮合模式）+ AdminServer（TCP 9900），
 * 等待 Python 管理界面连接后通过 JSON Lines 协议交互。
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
    hdf::TradeSystem tradeSystem;
    hdf::AdminServer adminServer(port);

    // 互斥锁：TradeSystem 非线程安全，所有操作需串行化
    std::mutex tradeMutex;

    // ---- 连接 TradeSystem → AdminServer（回报广播） ----
    tradeSystem.setSendToClient([&adminServer](const nlohmann::json &output) {
        std::cout << "[Client] " << output.dump() << std::endl;
        adminServer.broadcast(output);
    });

    // 纯撮合模式：不设置 sendToExchange_
    // 如果需要前置模式，可打开以下注释：
    // tradeSystem.setSendToExchange([](const nlohmann::json &output) {
    //     std::cout << "[Exchange] " << output.dump() << std::endl;
    // });

    // ---- 连接 AdminServer → TradeSystem（指令转发） ----
    adminServer.setOnOrder(
        [&tradeSystem, &tradeMutex](const nlohmann::json &orderJson) {
            std::lock_guard<std::mutex> lock(tradeMutex);
            std::cout << "[Admin] Order: " << orderJson.dump() << std::endl;
            tradeSystem.handleOrder(orderJson);
        });

    adminServer.setOnCancel(
        [&tradeSystem, &tradeMutex](const nlohmann::json &cancelJson) {
            std::lock_guard<std::mutex> lock(tradeMutex);
            std::cout << "[Admin] Cancel: " << cancelJson.dump() << std::endl;
            tradeSystem.handleCancel(cancelJson);
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
    std::cout << "=== Admin Server started on port " << port
              << " ===" << std::endl;
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
