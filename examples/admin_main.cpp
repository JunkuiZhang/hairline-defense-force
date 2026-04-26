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
 *   ./bin/admin_main              # 默认端口 32000
 *   ./bin/admin_main 9901         # 自定义端口
 *
 * 然后启动 Python 端：
 *   cd admin && bash start.sh
 */

#include "admin_server.h"
#include "trade_system.h"
#include <csignal>
#include <iostream>

static std::atomic<bool> g_running{true};

static void signalHandler(int /*sig*/) { g_running = false; }

int main(int argc, char *argv[]) {
    // 解析端口
    uint16_t port = 32000;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    // ---- 构建核心组件 ----
    hdf::TradeSystem gateway;  // 交易所前置系统
    hdf::TradeSystem exchange; // 纯撮合系统（模拟交易所）
    hdf::AdminServer adminServer(port);

    // ---- 连接 gateway → Admin UI（回报广播） ----
    gateway.setSendToClient(
        [&adminServer](const hdf::ClientReport &report) {
            nlohmann::json output = hdf::to_json_report(report);
            std::cout << "[→Client] " << output.dump() << std::endl;
            nlohmann::json gatewayResp = output;
            gatewayResp["type"] = "response";
            gatewayResp["source"] = "gateway";
            adminServer.broadcast(gatewayResp);
        });

    // ---- 连接 gateway → exchange（前置转发给交易所） ----
    // 根据 variant 类型区分订单和撤单
    gateway.setSendToExchange(
        [&exchange](const hdf::ExchangeRequest &req) {
            nlohmann::json j = hdf::to_json_request(req);
            std::cout << "[→Exchange] " << j.dump() << std::endl;
            std::visit(
                [&exchange](const auto &r) {
                    using T = std::decay_t<decltype(r)>;
                    if constexpr (std::is_same_v<T, hdf::CancelOrder>) {
                        nlohmann::json cancelJson;
                        hdf::to_json(cancelJson, r);
                        exchange.handleCancel(cancelJson);
                    } else {
                        nlohmann::json orderJson;
                        hdf::to_json(orderJson, r);
                        exchange.handleOrder(orderJson);
                    }
                },
                req);
        });

    // ---- 连接 exchange → gateway（交易所回报返回前置） ----
    // 同时广播交易所回报供监控（加 source 标记区分来源）
    exchange.setSendToClient(
        [&gateway, &adminServer](const hdf::ClientReport &report) {
            nlohmann::json resp = hdf::to_json_report(report);
            std::cout << "[←Exchange] " << resp.dump() << std::endl;
            // 广播交易所回报（加 source 标记供前端区分）
            nlohmann::json exchangeResp = resp;
            exchangeResp["type"] = "response";
            exchangeResp["source"] = "exchange";
            adminServer.broadcast(exchangeResp);
            // 回报也需要流入 gateway 处理
            hdf::ExchangeReport exchangeReport = resp.get<hdf::ExchangeReport>();
            gateway.handleResponse(exchangeReport);
        });
    // exchange 不设置 sendToExchange_ → 纯撮合模式

    // ---- 连接 exchange → gateway（行情推送） ----
    // 交易所订单簿变动时推送 best bid/ask，前置据此做行情约束
    exchange.setSendMarketData(
        [&gateway, &adminServer](const std::vector<hdf::MarketDataItem> &items) {
            for (const auto &item : items) {
                nlohmann::json data;
                data["market"] = hdf::to_string(item.market);
                data["securityId"] = item.securityId.str();
                data["bidPrice"] = item.bidPrice;
                data["askPrice"] = item.askPrice;
                std::cout << "[MarketData] " << data.dump() << std::endl;

                // 广播给管理界面显示
                nlohmann::json msg;
                msg["type"] = "market_data";
                msg["source"] = "exchange";
                msg["data"] = data;
                adminServer.broadcast(msg);
            }
            // 推送给前置系统做行情约束
            gateway.handleMarketData(items);
        });

    // ---- 连接 AdminServer → gateway/exchange（管理界面指令转发） ----
    // 根据 target 字段路由到 gateway（默认）或 exchange
    // 使用 submitOrder/submitCancel 投递到 MPSC 命令队列，线程安全
    adminServer.setOnOrder(
        [&gateway, &exchange](const nlohmann::json &orderJson) {
            std::string target = orderJson.value("target", "gateway");
            // 移除 target 字段，避免传入 TradeSystem
            nlohmann::json cleaned = orderJson;
            cleaned.erase("target");
            if (target == "exchange") {
                std::cout << "[Admin←] Order(→Exchange): " << cleaned.dump()
                          << std::endl;
                exchange.submitOrder(cleaned);
            } else {
                std::cout << "[Admin←] Order(→Gateway): " << cleaned.dump()
                          << std::endl;
                gateway.submitOrder(cleaned);
            }
        });

    adminServer.setOnCancel(
        [&gateway, &exchange](const nlohmann::json &cancelJson) {
            std::string target = cancelJson.value("target", "gateway");
            nlohmann::json cleaned = cancelJson;
            cleaned.erase("target");
            if (target == "exchange") {
                std::cout << "[Admin←] Cancel(→Exchange): " << cleaned.dump()
                          << std::endl;
                exchange.submitCancel(cleaned);
            } else {
                std::cout << "[Admin←] Cancel(→Gateway): " << cleaned.dump()
                          << std::endl;
                gateway.submitCancel(cleaned);
            }
        });

    adminServer.setOnQuery(
        [&gateway,
         &exchange](const nlohmann::json &queryMsg) -> nlohmann::json {
            // queryOrderbook 是 const 方法，在 eventLoop 运行时调用
            // 存在轻微的读写竞争，但对于快照查询可接受
            // 生产环境应通过 submitQuery + promise/future 模式解决
            std::string queryType = queryMsg.value("queryType", "orderbook");
            std::string secId = queryMsg.value("securityId", "");
            std::string mktStr = queryMsg.value("market", "");

            nlohmann::json result;
            result["queryType"] = queryType;
            if (queryType == "orderbook") {
                if (!secId.empty() && !mktStr.empty()) {
                    // 按证券+市场筛选，返回精确快照
                    hdf::Market mkt = hdf::market_from_string(mktStr);
                    hdf::SecurityId sid(secId);
                    result["gateway"] = gateway.queryOrderbook(sid, mkt);
                    result["exchange"] = exchange.queryOrderbook(sid, mkt);
                } else {
                    // 返回聚合快照
                    result["gateway"] = gateway.queryOrderbook();
                    result["exchange"] = exchange.queryOrderbook();
                }
            } else if (queryType == "stats") {
                auto gwBook = gateway.queryOrderbook();
                auto exBook = exchange.queryOrderbook();
                result["gateway"] = {
                    {"totalOrders", gwBook["totalOrders"]},
                    {"bidLevels", gwBook["bids"].size()},
                    {"askLevels", gwBook["asks"].size()},
                };
                result["exchange"] = {
                    {"totalOrders", exBook["totalOrders"]},
                    {"bidLevels", exBook["bids"].size()},
                    {"askLevels", exBook["asks"].size()},
                };
            } else {
                result["message"] = "Unknown query type: " + queryType;
            }
            return result;
        });

    // ---- 启动 ----
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    adminServer.start();
    gateway.startEventLoop();
    exchange.startEventLoop();
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
    gateway.stopEventLoop();
    exchange.stopEventLoop();
    std::cout << "Done." << std::endl;

    return 0;
}
