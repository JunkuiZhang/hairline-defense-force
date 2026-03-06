#pragma once

#include "matching_engine.h"
#include "risk_controller.h"
#include "trade_logger.h"
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace hdf {

/**
 * @brief 单个证券（或一组证券）的核心业务单元
 *
 * 拥有自己的风控器、撮合引擎和待处理状态。
 * TradeSystem 充当路由器，将命令分派到对应的 SecurityCore。
 *
 * @note 线程安全：SecurityCore 本身不是线程安全的，
 *       由外层（TradeSystem / WorkerBucket）保证串行访问。
 */
class SecurityCore {
  public:
    using SendToClient = std::function<void(const nlohmann::json &)>;
    using SendToExchange = std::function<void(const nlohmann::json &)>;
    using SendMarketData = std::function<void(const nlohmann::json &)>;

    SecurityCore();
    ~SecurityCore();

    // ─── 回调设置 ─────────────────────────────────────────
    void setSendToClient(SendToClient callback);
    void setSendToExchange(SendToExchange callback);
    void setSendMarketData(SendMarketData callback);

    /**
     * @brief 设置日志器（非拥有，由 TradeSystem 管理生命周期）
     */
    void setLogger(TradeLogger *logger);

    // ─── 业务处理接口 ─────────────────────────────────────
    void handleOrder(const nlohmann::json &input);
    void handleOrder(Order order);
    void handleCancel(const nlohmann::json &input);
    void handleCancel(CancelOrder order);
    void handleMarketData(const nlohmann::json &input);
    void handleResponse(const nlohmann::json &input);

    /**
     * @brief 获取内部订单簿快照（所有证券聚合）
     */
    nlohmann::json queryOrderbook() const;

    /**
     * @brief 获取指定证券的订单簿快照
     */
    nlohmann::json queryOrderbook(const std::string &securityId,
                                  Market market) const;

  private:
    RiskController riskController_;
    MatchingEngine matchingEngine_;
    TradeLogger *logger_ = nullptr; // 非拥有

    SendToClient sendToClient_;
    SendToExchange sendToExchange_;
    SendMarketData sendMarketData_;

    // ─── 前置模式待处理状态 ───────────────────────────────

    struct PendingMatch {
        Order activeOrder;
        std::vector<OrderResponse> executions;
        uint32_t remainingQty = 0;
        size_t pendingCancelCount = 0;
        std::unordered_set<std::string> confirmedIds;
        std::unordered_set<std::string> rejectedIds;
    };

    std::unordered_map<std::string, PendingMatch> pendingMatches_;
    std::unordered_map<std::string, std::string> cancelToActiveOrder_;

    struct PendingConfirm {
        Order activeOrder;
        std::vector<OrderResponse> confirmedExecutions;
    };

    std::unordered_map<std::string, PendingConfirm> pendingConfirms_;
    std::unordered_set<std::string> localOnlyOrders_;
    std::unordered_map<std::string, MarketData> latestMarketData_;

    // ─── 内部方法 ─────────────────────────────────────────
    void resolvePendingMatch(const std::string &activeOrderId);
    void
    sendConfirmAndExecReports(const Order &activeOrder,
                              const std::vector<OrderResponse> &executions);
    void broadcastMarketData(const std::string &securityId, Market market);
};

} // namespace hdf
