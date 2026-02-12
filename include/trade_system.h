#pragma once

#include "matching_engine.h"
#include "risk_controller.h"
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace hdf {

/** 交易指令流转流程：
 *
 * ┌──────────┐   op1:订单/撤单   ┌──────────┐   op2:订单/撤单    ┌──────────┐
 * │  用户端   │ ───────────────> │  此系统   │ ───────────────> │  交易所   │
 * └──────────┘                  └──────────┘                  └──────────┘
 *      ↑                           │   ↑                            │
 *      │                           │   │                            │
 *      └───────── op4: 回报 ────────┘   └───────── op3: 回报 ────────┘
 **/
class TradeSystem {
  public:
    TradeSystem();
    ~TradeSystem();

    using SendToClient = std::function<void(const nlohmann::json &)>;
    using SendToExchange = std::function<void(const nlohmann::json &)>;

    /**
     * @brief 设置与客户端的交互接口，图中op4
     */
    void setSendToClient(SendToClient callback);
    /**
     * @brief 设置与交易所的交互接口，图中op2
     */
    void setSendToExchange(SendToExchange callback);

    /**
     * @brief 处理来自客户端的订单指令，图中op1
     */
    void handleOrder(const nlohmann::json &input);
    /**
     * @brief 处理来自客户端的撤单指令，图中op1
     */
    void handleCancel(const nlohmann::json &input);
    void handleMarketData(const nlohmann::json &input);
    /**
     * @brief 处理来自交易所的回报，图中op3
     */
    void handleResponse(const nlohmann::json &input);

  private:
    RiskController riskController_;
    MatchingEngine matchingEngine_;

    // 以下是系统与客户端和交易所交互的接口，系统可以根据是否设置了
    // sendToExchange_来判断自己是交易所前置还是纯撮合系统。
    SendToClient sendToClient_;
    SendToExchange sendToExchange_;

    /**
     * 前置模式下内部撮合成功后，需要先向交易所发送撤单请求，
     * 等待交易所返回所有撤单确认后才能向客户端发送成交回报。
     *
     * 一个主动方订单可能匹配多个对手方订单，要等所有对手方的
     * 撤单回报都回来后，才能确定最终成交结果：
     * - 撤单确认的部分 → 成交生效，发成交回报
     * - 撤单被拒的部分 → 对手方已在交易所被他人成交，该部分作废
     * - 若有作废部分未成交的量，需重新转发给交易所
     */
    struct PendingMatch {
        Order activeOrder;                     // 主动方订单（新来的订单）
        nlohmann::json activeOrderRawInput;    // 主动方原始JSON（转发用）
        std::vector<OrderResponse> executions; // 本次撮合产生的所有成交
        uint32_t remainingQty = 0;             // 撮合后未成交的剩余数量
        size_t pendingCancelCount = 0;         // 还在等待多少个撤单回报
        std::unordered_set<std::string>
            confirmedIds;                            // 已确认撤回的对手方订单ID
        std::unordered_set<std::string> rejectedIds; // 撤单被拒的对手方订单ID
    };

    // key: 主动方订单的 clOrderId
    std::unordered_map<std::string, PendingMatch> pendingMatches_;
    // 反向映射: 对手方订单ID → 主动方订单ID，用于收到撤单回报时查找归属
    std::unordered_map<std::string, std::string> cancelToActiveOrder_;

    /**
     * @brief 所有撤单回报都回来后，处理最终结果
     */
    void resolvePendingMatch(const std::string &activeOrderId);
};

} // namespace hdf
