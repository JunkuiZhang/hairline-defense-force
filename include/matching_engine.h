#pragma once

#include "types.h"
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
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
     * 此函数为纯匹配操作，不会自动入簿。
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
    /**
     * @brief 订单簿中的订单条目，记录订单信息及已成交累计量。
     *
     * 使用 remainingQty 表示当前剩余可成交数量，
     * cumQty 记录已成交的累计数量，用于撤单回报。
     */
    struct BookEntry {
        Order order;               // 原始订单信息
        uint32_t remainingQty = 0; // 剩余可成交数量
        uint32_t cumQty = 0;       // 已成交累计数量
    };

    /**
     * @brief 同一价格档位上的订单队列（时间优先）。
     *
     * 使用 std::list 保持插入顺序（即时间优先），
     * 同时支持高效的中间删除操作。
     */
    using PriceLevel = std::list<BookEntry>;

    /**
     * @brief 买方订单簿：按价格降序排列。
     *
     * 使用 std::map<double, PriceLevel, std::greater<double>>，
     * key 为价格，value 为该价格下的订单队列。
     * std::greater 确保价格从高到低排列（买方优先匹配高价）。
     */
    std::map<double, PriceLevel, std::greater<double>> bidBook_;

    /**
     * @brief 卖方订单簿：按价格升序排列。
     *
     * 使用 std::map<double, PriceLevel>（默认 std::less），
     * key 为价格，value 为该价格下的订单队列。
     * 价格从低到高排列（卖方优先匹配低价）。
     */
    std::map<double, PriceLevel> askBook_;

    /**
     * @brief 订单ID到订单簿位置的反向索引。
     *
     * 用于 cancelOrder 和 reduceOrderQty 快速定位订单，
     * 避免遍历整个订单簿。存储了订单所在的价格和买卖方向。
     */
    struct OrderLocation {
        double price; // 订单价格（用于在 bidBook_/askBook_ 中定位）
        Side side;    // 买卖方向（决定查 bidBook_ 还是 askBook_）
    };
    std::unordered_map<std::string, OrderLocation> orderIndex_;

    /**
     * @brief 全局成交编号计数器，用于生成唯一的 execId。
     */
    uint64_t nextExecId_ = 1;

    /**
     * @brief 生成唯一的成交编号。
     *
     * @return 格式为 "EXEC" + 16位数字的唯一编号字符串。
     */
    std::string generateExecId();
};

} // namespace hdf
