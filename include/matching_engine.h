#pragma once

#include "fast_hashmap.h"
#include "lob.h"
#include "types.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace hdf {

/**
 * @brief 撮合引擎，负责订单簿管理、订单撮合、撤单等操作。
 *
 * 内部按 (market, securityId) 分别维护独立订单簿，撮合时直接定位目标订单簿，
 * 无需遍历跳过不相关证券，消除了 securityId 字符串比较开销。
 *
 * 订单簿基于 LOB (Limit Order Book) 设计：
 *   - OrderPool 预分配对象池 + freelist，O(1) 分配/回收
 *   - PriceLevel 侵入式双向链表，O(1) 头尾插入/删除
 *   - 价格离散化 (tick=0.01)，O(1) 价位定位
 *
 * @note 线程安全：此类不是线程安全的。
 */
class MatchingEngine {
  public:
    MatchingEngine();
    ~MatchingEngine();

    struct MatchResult {
        std::vector<OrderResponse> executions;
        uint32_t remainingQty = 0;
    };

    /**
     * @brief 尝试将订单与订单簿中的订单进行撮合。
     * 此函数为纯匹配操作，不会自动入簿。
     */
    [[gnu::hot]]
    MatchResult
    match(const Order &order,
          const std::optional<MarketData> &marketData = std::nullopt);

    [[gnu::hot]]
    void addOrder(const Order &order);

    [[gnu::hot]]
    CancelResponse cancelOrder(const OrderId &clOrderId);

    void reduceOrderQty(const OrderId &clOrderId, uint32_t qty);

    bool hasOrder(const OrderId &clOrderId);

    nlohmann::json getSnapshot();

    nlohmann::json getSnapshot(const SecurityId &securityId,
                               Market market);

    MarketData getBestQuote(const SecurityId &securityId, Market market);

  private:
    static BookKey makeBookKey(const SecurityId &securityId, Market market) {
        return makeRouteKey(market, securityId);
    }

    // ─── LOB 订单簿结构 ──────────────────────────────────────

    /// 单个 (market, securityId) 的订单簿，包含 bid 和 ask 两侧
    struct SecurityBook {
        OrderBook bidBook;
        OrderBook askBook;

        void init(size_t pool_capacity_per_side, double base_price,
                  size_t level_count) {
            bidBook.init(pool_capacity_per_side, base_price, level_count);
            bidBook.set_bid_side(true);
            askBook.init(pool_capacity_per_side, base_price, level_count);
            askBook.set_bid_side(false);
        }
    };

    /// 所有证券+市场的订单簿集合
    hdf::FastHashmap<BookKey, SecurityBook> books_;

    /// 订单ID到订单簿位置的反向索引
    struct OrderLocation {
        BookKey bookKey;
        Side side;
        size_t pool_index; // Order 在 OrderBook pool 中的索引
    };
    hdf::FastHashmap<OrderId, OrderLocation> orderIndex_;

    /// 全局成交编号计数器
    uint64_t nextExecId_ = 1;
    ExecIdStr generateExecId();

    // ─── 配置 ────────────────────────────────────────────────
    static constexpr size_t kPoolCapacityPerSide = 500000;
    static constexpr double kBasePrice = 0.0;
    static constexpr size_t kLevelCount = 500001; // 0.00 ~ 5000.00
};

} // namespace hdf
