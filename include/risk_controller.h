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
     * @brief 订单信息结构体。
     *
     * 存储订单的关键信息，用于对敲检测。
     */
    struct OrderInfo {
        std::string clOrderId;  // 客户订单ID
        std::string securityId; // 股票代码
        Side side;              // 买卖方向（BUY/SELL）
        double price;           // 订单价格
        uint32_t remainingQty;  // 剩余未成交数量
    };

    // 买卖方向 -> 订单列表的映射
    using SideOrders = std::unordered_map<Side, std::vector<OrderInfo>>;

    // 股票代码 -> 买卖方订单的映射
    using SecurityOrders = std::unordered_map<std::string, SideOrders>;

    // 股东号 -> 股票订单的映射
    using ShareholderOrders = std::unordered_map<std::string, SecurityOrders>;

    // 活跃订单的三层索引结构
    // 结构：股东号 -> 股票代码 -> 买卖方向 -> 订单列表
    ShareholderOrders activeOrders_;
};

} // namespace hdf
