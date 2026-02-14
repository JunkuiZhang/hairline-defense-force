#pragma once

#include "types.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace hdf {

class RiskController {
  public:
    enum class RiskCheckResult {
        PASSED,      // 风控检查通过
        CROSS_TRADE, // 检测到对敲风险
    };

    RiskController();
    ~RiskController();

    /**
     * @brief 检查订单是否符合风控要求。
     *
     * @param order 要检查的订单。
     * @return RiskCheckResult PASSED 或 CROSS_TRADE
     */
    RiskCheckResult checkOrder(const Order &order);

    /**
     * @brief 检查订单是否会导致对敲交易。
     *
     * 对敲条件：相同股东号 + 相同股票 + 相反方向 + 反方向订单有剩余数量
     *
     * @param order 要检查的订单。
     * @return true 检测到对敲，false 未检测到对敲
     */
    bool isCrossTrade(const Order &order);

    /**
     * @brief 订单被接受时的回调。
     *
     * 将订单添加到内部索引结构中，用于后续的对敲检测。
     *
     * @param order 被接受的订单。
     */
    void onOrderAccepted(const Order &order);

    /**
     * @brief 订单被撤销时的回调。
     *
     * 从内部索引结构中移除指定订单。
     *
     * @param origClOrderId 原始订单的客户订单ID。
     */
    void onOrderCanceled(const std::string &origClOrderId);

    /**
     * @brief 订单成交时的回调。
     *
     * 更新订单的剩余数量，如果完全成交则后续不再参与对敲检测。
     *
     * @param clOrderId 客户订单ID。
     * @param execQty 成交数量。
     */
    void onOrderExecuted(const std::string &clOrderId, uint32_t execQty);

  private:
    /**
     * @brief 订单详细信息，用于内部维护状态。
     */
    struct OrderInfo {
        std::string clOrderId;     // 客户订单ID
        std::string shareholderId; // 股东ID
        Market market;             // 交易市场
        std::string securityId;    // 股票代码
        Side side;                 // 买卖方向
        double price;              // 订单价格
        uint32_t remainingQty;     // 剩余未成交数量
    };

    // 辅助函数：生成组合键
    // Key: shareholderId + securityId + market
    std::string makeKey(const std::string &shareholderId, Market market,
                        const std::string &securityId);

    // 组合键 -> 该方向的总挂单数量
    // 只有买单会存入 buySide_，卖单存入 sellSide_
    // 用于 O(1) 快速检查反方向是否存在挂单量
    std::unordered_map<std::string, uint32_t> buySide_;
    std::unordered_map<std::string, uint32_t> sellSide_;

    // orderId -> 订单详细信息，用于快速查找订单归属
    // 用于撤单和成交时快速定位订单
    std::unordered_map<std::string, OrderInfo> orderMap_;
};

} // namespace hdf
