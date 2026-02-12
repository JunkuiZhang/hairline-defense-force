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
     * 此函数为纯匹配操作，不会修改订单簿状态（不会自动入簿）。
     * 匹配到的对手方订单会从订单簿中移除或减少数量。
     * 调用方需根据返回的 remainingQty 自行决定是入簿还是转发。
     *
     * @param order 要撮合的订单。
     * @param marketData 可选的市场数据输入，用于更复杂的撮合逻辑。
     * @return MatchResult
     * 包含撮合结果和剩余未成交数量。如果无法匹配，返回 nullopt。
     */
    std::optional<MatchResult>
    match(const Order &order,
          const std::optional<MarketData> &marketData = std::nullopt);

    /**
     * @brief 添加订单到内部订单簿。
     * 由调用方在合适的时机显式调用此函数入簿。
     * 支持传入修改后的数量（如部分成交后的剩余量）。
     */
    void addOrder(const Order &order);

    /**
     * @brief 从内部订单簿中移除订单。
     */
    CancelResponse cancelOrder(const std::string &clOrderId);

    /**
     * @brief 减少订单簿中指定订单的数量。
     * 用于交易所主动成交后同步内部订单簿状态。
     * 若减少后数量为0，则从订单簿中移除该订单。
     *
     * @param clOrderId 订单的唯一编号。
     * @param qty 要减少的数量。
     */
    void reduceOrderQty(const std::string &clOrderId, uint32_t qty);

  private:
    // 订单簿
    // std::map<std::string, std::vector<Order>> orderBook_;
};

} // namespace hdf
