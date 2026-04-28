#pragma once

#include "types.h"
#include <cassert>
#include <cstddef>
#include <optional>
#include <vector>

namespace hdf {

// ============================================================
// OrderPool — 预分配的 Order 对象池 + freelist
// O(1) 分配/回收，零堆分配（初始 resize 后）
// ============================================================
class OrderPool {
  public:
    OrderPool() {}

    void set_capacity(size_t capacity) {
        head = 0;
        storage.resize(capacity);
        for (size_t x = 0; x < capacity; x++) {
            Order &o = storage[x];
            o.prev = std::nullopt;
            o.next = x + 1;
        }
        storage[capacity - 1].next = std::nullopt;
    }

    /// 从 freelist 分配一个槽位，将 order 写入并返回索引
    std::optional<size_t> allocate(const Order &order) {
        if (!head.has_value()) {
            return std::nullopt;
        }
        size_t idx = head.value();
        Order &slot = storage[idx];
        std::optional<size_t> next_free = slot.next;
        slot = order;
        slot.prev = std::nullopt;
        slot.next = std::nullopt;
        head = next_free;
        return idx;
    }

    /// 回收一个槽位到 freelist
    void deallocate(size_t node_index) {
        Order &slot = storage[node_index];
        slot.prev = std::nullopt;
        slot.next = head;
        head = node_index;
    }

    Order &at(size_t index) { return storage[index]; }
    const Order &at(size_t index) const { return storage[index]; }

  private:
    std::vector<Order> storage;
    std::optional<size_t> head;
};

// ============================================================
// PriceLevel — 单个价位的订单链表头/尾 + 总量
// ============================================================
struct PriceLevel {
    std::optional<size_t> head;
    std::optional<size_t> tail;
    uint32_t total_volume = 0;
    uint32_t order_count = 0;
};

// ============================================================
// PriceBitSet — O(1) 价位扫描加速位图
// ============================================================
class PriceBitSet {
  public:
    PriceBitSet() = default;

    void init(size_t capacity) {
        words.assign((capacity + 63) / 64, 0);
    }

    /// 标记某个价格档位有单
    inline void set(size_t index) {
        words[index / 64] |= (1ULL << (index % 64));
    }

    /// 标记某个价格档位空了
    inline void clear(size_t index) {
        words[index / 64] &= ~(1ULL << (index % 64));
    }

    /// 寻找 <= 当前 index 的最高有单价位 (买一撤单或向下遍历时用)
    inline std::optional<size_t> find_prev(size_t index) const {
        if (words.empty()) return std::nullopt;
        size_t word_idx = index / 64;
        if (word_idx >= words.size()) word_idx = words.size() - 1;
        size_t bit_idx = index % 64;
        
        uint64_t word = words[word_idx];
        // 屏蔽高于 bit_idx 的位 (保留 <= bit_idx 的位)
        uint64_t mask = (bit_idx == 63) ? ~0ULL : ((1ULL << (bit_idx + 1)) - 1);
        word &= mask;

        if (word != 0) {
            return word_idx * 64 + 63 - __builtin_clzll(word);
        }

        if (word_idx == 0) return std::nullopt;

        for (size_t w = word_idx - 1; w != (size_t)-1; --w) {
            if (words[w] != 0) {
                return w * 64 + 63 - __builtin_clzll(words[w]);
            }
        }
        return std::nullopt;
    }

    /// 寻找 >= 当前 index 的最低有单价位 (卖一撤单或向上遍历时用)
    inline std::optional<size_t> find_next(size_t index) const {
        if (words.empty()) return std::nullopt;
        size_t word_idx = index / 64;
        if (word_idx >= words.size()) return std::nullopt;
        size_t bit_idx = index % 64;

        uint64_t word = words[word_idx];
        // 屏蔽低于 bit_idx 的位
        uint64_t mask = ~((1ULL << bit_idx) - 1);
        word &= mask;

        if (word != 0) {
            return word_idx * 64 + __builtin_ctzll(word);
        }

        for (size_t w = word_idx + 1; w < words.size(); ++w) {
            if (words[w] != 0) {
                return w * 64 + __builtin_ctzll(words[w]);
            }
        }
        return std::nullopt;
    }

  private:
    std::vector<uint64_t> words;
};

// ============================================================
// OrderBook — 单侧（bid 或 ask）的订单簿
//
// 使用价格离散化 (tick=0.01) 将 levels 向量作为 O(1) 数组索引。
// 通过 PriceBitSet 实现 O(1) 的最优价查找和遍历跳跃。
// ============================================================
class OrderBook {
  public:
    OrderBook() = default;

    void init(size_t pool_capacity, double base_price, size_t level_count) {
        pool.set_capacity(pool_capacity);
        this->base_price = base_price;
        levels.resize(level_count);
        bitset.init(level_count);
    }

    // ─── 价位索引 ───────────────────────────────────────────

    [[gnu::hot]]
    inline size_t price_to_index(double price) const noexcept {
        int idx = static_cast<int>((price - base_price) * 100.0 + 0.5);
        assert(idx >= 0 && static_cast<size_t>(idx) < levels.size());
        return static_cast<size_t>(idx);
    }

    inline double index_to_price(size_t index) const {
        return base_price + index * 0.01;
    }

    // ─── 插入 ───────────────────────────────────────────────

    /// 插入订单，返回 pool 中的索引
    [[gnu::hot]]
    std::optional<size_t> insert(const Order &order) {
        auto opt = pool.allocate(order);
        if (!opt.has_value())
            return std::nullopt;
        size_t idx = opt.value();
        Order &stored = pool.at(idx);
        stored.remainingQty = order.qty;
        stored.cumQty = 0;
        stored.prev = std::nullopt;
        stored.next = std::nullopt;

        size_t lvl_idx = price_to_index(order.price);
        PriceLevel &lvl = levels[lvl_idx];

        // 追加到链表尾部
        if (lvl.tail.has_value()) {
            pool.at(lvl.tail.value()).next = idx;
            stored.prev = lvl.tail.value();
        } else {
            lvl.head = idx;
        }
        lvl.tail = idx;
        lvl.total_volume += order.qty;
        lvl.order_count++;

        bitset.set(lvl_idx);

        if (!best_index.has_value()) {
            best_index = lvl_idx;
        } else {
            if (bid_side) {
                if (lvl_idx > best_index.value()) best_index = lvl_idx;
            } else {
                if (lvl_idx < best_index.value()) best_index = lvl_idx;
            }
        }

        return idx;
    }

    // ─── 移除 ───────────────────────────────────────────────

    /// 按 pool 索引移除订单（O(1)）
    [[gnu::hot]]
    void remove(size_t pool_index) {
        Order &o = pool.at(pool_index);
        size_t lvl_idx = price_to_index(o.price);
        PriceLevel &lvl = levels[lvl_idx];

        // 断开双向链表
        if (o.prev.has_value()) {
            pool.at(o.prev.value()).next = o.next;
        } else {
            lvl.head = o.next;
        }
        if (o.next.has_value()) {
            pool.at(o.next.value()).prev = o.prev;
        } else {
            lvl.tail = o.prev;
        }

        lvl.total_volume -=
            (o.remainingQty <= lvl.total_volume) ? o.remainingQty : lvl.total_volume;
        lvl.order_count--;

        pool.deallocate(pool_index);

        if (!lvl.head.has_value()) {
            bitset.clear(lvl_idx);
            
            if (best_index.has_value() && best_index.value() == lvl_idx) {
                if (bid_side) {
                    if (lvl_idx > 0) {
                        best_index = bitset.find_prev(lvl_idx - 1);
                    } else {
                        best_index = std::nullopt;
                    }
                } else {
                    best_index = bitset.find_next(lvl_idx + 1);
                }
            }
        }
    }

    // ─── 访问 ───────────────────────────────────────────────

    Order &order_at(size_t pool_index) { return pool.at(pool_index); }
    const Order &order_at(size_t pool_index) const {
        return pool.at(pool_index);
    }

    PriceLevel &level_at(size_t lvl_index) { return levels[lvl_index]; }
    const PriceLevel &level_at(size_t lvl_index) const {
        return levels[lvl_index];
    }

    size_t level_count() const { return levels.size(); }

    /// 设置扫描方向：bid 从高到低搜，ask 从低到高搜
    void set_bid_side(bool is_bid) { bid_side = is_bid; }

    // ─── 快速遍历 ─────────────────────────────────────────

    std::optional<size_t> best_level_index() const { return best_index; }
    
    std::optional<double> best_price() const {
        if (!best_index.has_value())
            return std::nullopt;
        return index_to_price(best_index.value());
    }

    std::optional<size_t> next_level(size_t current_idx) const {
        if (bid_side) {
            if (current_idx == 0) return std::nullopt;
            return bitset.find_prev(current_idx - 1);
        } else {
            return bitset.find_next(current_idx + 1);
        }
    }

  private:
    OrderPool pool;
    std::vector<PriceLevel> levels;
    PriceBitSet bitset;
    
    double base_price = 0.0;
    bool bid_side = false;
    std::optional<size_t> best_index;
};

} // namespace hdf