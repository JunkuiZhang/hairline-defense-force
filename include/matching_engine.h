#pragma once

#include "types.h"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "memory_pool.h"

namespace hdf {

/**
 * @brief 撮合引擎，负责订单簿管理、订单撮合、撤单等操作。
 *
 * 内部按 (market, securityId) 分别维护独立订单簿，撮合时直接定位目标订单簿，
 * 无需遍历跳过不相关证券，消除了 securityId 字符串比较开销。
 *
 * @note 线程安全：此类不是线程安全的。所有对 MatchingEngine
 *       实例的访问必须在外部进行同步。对于高性能交易系统，
 *       建议使用单线程事件循环来串行化对引擎的访问。
 */
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
     * 包含撮合结果和剩余未成交数量。无成交时 executions 为空，
     * remainingQty 等于原始订单数量。
     */
    MatchResult
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
    CancelResponse cancelOrder(const OrderId &clOrderId);

    /**
     * @brief 减少订单簿中指定订单的数量。
     * 用于交易所主动成交后同步内部订单簿状态。
     * 若减少后数量为0，则从订单簿中移除该订单。
     *
     * @param clOrderId 订单的唯一编号。
     * @param qty 要减少的数量。
     */
    void reduceOrderQty(const OrderId &clOrderId, uint32_t qty);

    /**
     * @brief 查询指定订单是否仍在订单簿中。
     *
     * @param clOrderId 订单的唯一编号。
     * @return true 如果订单仍在订单簿中。
     */
    bool hasOrder(const OrderId &clOrderId) const;

    /**
     * @brief 获取所有证券的订单簿快照，返回买卖盘口的价格档位信息（聚合）。
     *
     * 跨所有 (market, securityId) 聚合后，买盘按价格降序、卖盘按价格升序，
     * 每档含价格、总量和累积量。
     *
     * @return nlohmann::json 包含 bids 和 asks 数组的 JSON 对象。
     */
    nlohmann::json getSnapshot() const;

    /**
     * @brief 获取指定证券+市场的订单簿快照。
     *
     * @param securityId 证券代码
     * @param market 市场
     * @return nlohmann::json 包含 bids 和 asks 数组的 JSON 对象。
     */
    nlohmann::json getSnapshot(const SecurityId &securityId,
                               Market market) const;

    /**
     * @brief 获取指定证券的最优买卖报价（best bid / best ask）。
     *
     * 直接定位该证券的 SecurityBook，O(1) 查找后取首档价格。
     *
     * @param securityId 证券代码
     * @param market 市场（如 XSHG、XHKG）
     * @return MarketData 包含 bidPrice 和 askPrice
     */
    MarketData getBestQuote(const SecurityId &securityId, Market market) const;

  private:
    /**
     * @brief 生成订单簿的查找 key。
     *
     * 格式为 "MARKET+securityId"，例如 "XSHG+600000"。
     * 每个 (market, securityId) 对应一个独立的 SecurityBook，
     * 保证不同市场或不同证券的订单完全隔离，撮合时无需 securityId 比较。
     */
    static BookKey makeBookKey(const SecurityId &securityId,
                               Market market) {
        std::string key = to_string(market) + "+" + std::string(securityId);
        return BookKey(key);
    }

    struct BookEntry {
        Order order;
        uint32_t remainingQty = 0;
        uint32_t cumQty = 0;
        BookEntry* next = nullptr;
        BookEntry* prev = nullptr;
    };

    /**
     * @brief 同一价格档位上的订单队列（采用侵入式双向链表消除分配）
     */
    class PriceLevel {
    public:
        BookEntry* head = nullptr;
        BookEntry* tail = nullptr;

        bool empty() const { return head == nullptr; }

        void push_back(BookEntry* entry) {
            entry->next = nullptr;
            entry->prev = tail;
            if (tail) tail->next = entry;
            else head = entry;
            tail = entry;
        }

        BookEntry* erase_node(BookEntry* entry) {
            BookEntry* next = entry->next;
            if (entry->prev) entry->prev->next = entry->next;
            else head = entry->next;
            if (entry->next) entry->next->prev = entry->prev;
            else tail = entry->prev;
            return next;
        }

        class iterator {
        public:
            BookEntry* ptr;
            iterator(BookEntry* p) : ptr(p) {}
            BookEntry& operator*() { return *ptr; }
            BookEntry* operator->() { return ptr; }
            iterator& operator++() { ptr = ptr->next; return *this; }
            bool operator!=(const iterator& other) const { return ptr != other.ptr; }
            bool operator==(const iterator& other) const { return ptr == other.ptr; }
        };

        iterator begin() { return iterator(head); }
        iterator end() { return iterator(nullptr); }

        class const_iterator {
        public:
            const BookEntry* ptr;
            const_iterator(const BookEntry* p) : ptr(p) {}
            const BookEntry& operator*() const { return *ptr; }
            const BookEntry* operator->() const { return ptr; }
            const_iterator& operator++() { ptr = ptr->next; return *this; }
            bool operator!=(const const_iterator& other) const { return ptr != other.ptr; }
            bool operator==(const const_iterator& other) const { return ptr == other.ptr; }
        };

        const_iterator begin() const { return const_iterator(head); }
        const_iterator end() const { return const_iterator(nullptr); }

        iterator erase(iterator it) {
            return iterator(erase_node(it.ptr));
        }
    };

    /**
     * @brief 价格桶：价格 + 该价格档的订单链表。
     */
    struct PriceBucket {
        uint64_t price;
        PriceLevel level;
    };

    /**
     * @brief 扁平化订单簿：使用 sorted vector 替代 std::map。
     *
     * 买盘降序排列（最优买价在 front），卖盘升序排列（最优卖价在 front）。
     * 撮合时从 front 顺序扫描，天然 cache-friendly。
     *
     * @tparam Descending true = 降序（买盘），false = 升序（卖盘）
     */
    template <bool Descending>
    class FlatBook {
    public:
        std::vector<PriceBucket> buckets_;

        bool empty() const { return buckets_.empty(); }
        size_t size() const { return buckets_.size(); }

        // 返回 front（最优价格端）
        PriceBucket& front() { return buckets_.front(); }
        const PriceBucket& front() const { return buckets_.front(); }

        using iterator = typename std::vector<PriceBucket>::iterator;
        using const_iterator = typename std::vector<PriceBucket>::const_iterator;

        iterator begin() { return buckets_.begin(); }
        iterator end() { return buckets_.end(); }
        const_iterator begin() const { return buckets_.begin(); }
        const_iterator end() const { return buckets_.end(); }
        iterator erase(iterator it) { return buckets_.erase(it); }

        /**
         * @brief 查找指定价格的 PriceLevel，找不到返回 end()。
         * 二分查找 O(log N)。
         */
        iterator find(uint64_t price) {
            auto it = lowerBound(price);
            if (it != buckets_.end() && it->price == price)
                return it;
            return buckets_.end();
        }

        /**
         * @brief 获取或创建指定价格的 PriceLevel。
         * 二分查找定位插入点，不存在则原地 insert。
         */
        PriceLevel& operator[](uint64_t price) {
            auto it = lowerBound(price);
            if (it != buckets_.end() && it->price == price)
                return it->level;
            auto inserted = buckets_.insert(it, PriceBucket{price, {}});
            return inserted->level;
        }

    private:
        /**
         * @brief lower_bound：找到第一个 "不优于" price 的位置。
         * Descending 模式下：找第一个 <= price 的位置（降序，越大越优）
         * Ascending 模式下：找第一个 >= price 的位置（升序，越小越优）
         */
        iterator lowerBound(uint64_t price) {
            if constexpr (Descending) {
                // 降序：我们要找第一个 bucket.price <= price
                return std::lower_bound(
                    buckets_.begin(), buckets_.end(), price,
                    [](const PriceBucket& b, uint64_t p) {
                        return b.price > p; // skip while bucket is more optimal
                    });
            } else {
                // 升序：我们要找第一个 bucket.price >= price
                return std::lower_bound(
                    buckets_.begin(), buckets_.end(), price,
                    [](const PriceBucket& b, uint64_t p) {
                        return b.price < p;
                    });
            }
        }
    };

    /**
     * @brief 单个 (market, securityId) 的订单簿。
     *
     * bidBook 按价格降序（sorted vector），askBook 按价格升序（sorted vector）。
     * 只存放同一证券的订单，撮合时无需跳过不相关条目。
     */
    struct SecurityBook {
        FlatBook<true>  bidBook;  // 降序：最优买价在 front
        FlatBook<false> askBook;  // 升序：最优卖价在 front
    };

    /**
     * @brief 所有证券+市场的订单簿集合。
     * key = makeBookKey(securityId, market)
     */
    std::unordered_map<BookKey, SecurityBook> books_;

    /**
     * @brief 订单对象池，消除对象生成时的分配
     */
    ObjectPool<BookEntry> entryPool_;

    /**
     * @brief 订单ID到订单簿位置的反向索引。
     */
    struct OrderLocation {
        BookKey bookKey; // 所属 SecurityBook 的 key
        uint64_t price;
        Side side;
    };
    std::unordered_map<OrderId, OrderLocation> orderIndex_;

    /**
     * @brief 全局成交编号计数器。
     */
    uint64_t nextExecId_ = 1;
    ExecId generateExecId();
};

} // namespace hdf
