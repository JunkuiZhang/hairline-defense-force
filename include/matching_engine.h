#pragma once

#include "types.h"
#include <optional>
#include <vector>

namespace hdf {

class MatchingEngine {
  public:
    MatchingEngine();
    ~MatchingEngine();

    struct MatchResult {
        std::vector<OrderResponse> executions; // 有可能匹配多个订单
        uint32_t remainingQty = 0;             // 未成交剩余数量
    };

    /**
     * @brief 尝试将订单与订单簿中的订单进行撮合。
     *
     * @param order 要撮合的订单。
     * @param marketData 可选的市场数据输入，用于更复杂的撮合逻辑。
     * @return MatchResult
     * 包含撮合结果和剩余未成交数量。如果订单撮合失败，executions将为空。
     */
    std::optional<MatchResult>
    match(const Order &order,
          const std::optional<MarketData> &marketData = std::nullopt);

    /**
     * @brief 添加订单到内部订单簿。
     */
    void addOrder(const Order &order);

    /**
     * @brief 从内部订单簿中移除订单。
     */
    CancelResponse cancelOrder(const std::string &clOrderId);

  private:
    // 订单簿
    // std::map<std::string, std::vector<Order>> orderBook_;
};

} // namespace hdf
