#include "matching_engine.h"
#include "types.h"
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <map>

namespace hdf {

MatchingEngine::MatchingEngine() {
    books_.set_capacity(1000);
    orderIndex_.set_capacity(1000000);
}
MatchingEngine::~MatchingEngine() {}

// ============================================================
// B9: execId 生成
// ============================================================
ExecIdStr MatchingEngine::generateExecId() {
    const uint64_t currentId = nextExecId_++ % 10000000000000000ULL;
    ExecIdStr id;
    std::snprintf(id.data, sizeof(id.data), "EXEC%016llu",
                  (unsigned long long)currentId);
    return id;
}


// ============================================================
// B2: addOrder
// ============================================================
void MatchingEngine::addOrder(const Order &order) {
    if (orderIndex_.get(order.clOrderId) != nullptr)
        return;

    const BookKey bookKey = makeBookKey(order.securityId, order.market);
    SecurityBook &sb = books_[bookKey];

    // 懒初始化：检查 bidBook 是否已初始化（level_count == 0）
    if (sb.bidBook.level_count() == 0) {
        sb.init(kPoolCapacityPerSide, kBasePrice, kLevelCount);
    }

    OrderBook &book =
        (order.side == Side::BUY) ? sb.bidBook : sb.askBook;

    auto opt = book.insert(order);
    if (!opt.has_value()) {
        std::cerr << "[MatchingEngine] CRITICAL: OrderPool exhausted\n";
        return;
    }

    orderIndex_[order.clOrderId] = {bookKey, order.side, opt.value()};
}

// ============================================================
// B3-B6: match
//
// 撮合规则：
//   - 价格优先：买单优先匹配最低卖价，卖单优先匹配最高买价
//   - 时间优先：同价格先挂单先成交（链表头部先匹配）
//   - 成交价：以被动方（maker）的挂单价格作为成交价
//   - 部分成交：一笔订单可匹配多个对手方，逐个消耗数量
//   - 零股处理：买入单必须为100股整数倍，卖出单可以不是
// ============================================================
MatchingEngine::MatchResult
MatchingEngine::match(const Order &order,
                      const std::optional<MarketData> &marketData) {
    MatchResult result;
    uint32_t remainingQty = order.qty;

    const BookKey bookKey = makeBookKey(order.securityId, order.market);
    auto *sbPtr = books_.get(bookKey);
    if (sbPtr == nullptr) {
        result.remainingQty = remainingQty;
        return result;
    }
    SecurityBook &sb = *sbPtr;

    // 选择对手方订单簿
    // 买单 → 与 askBook 撮合；卖单 → 与 bidBook 撮合
    OrderBook &counterBook =
        (order.side == Side::BUY) ? sb.askBook : sb.bidBook;

    // 从最优价位开始，逐级遍历
    auto startOpt = counterBook.best_level_index();
    if (!startOpt.has_value()) {
        result.remainingQty = remainingQty;
        return result;
    }

    size_t lvl_idx = startOpt.value();

    while (remainingQty > 0) {
        PriceLevel &lvl = counterBook.level_at(lvl_idx);

        double counterPrice = counterBook.index_to_price(lvl_idx);

        // 价格检查
        if (order.side == Side::BUY) {
            if (order.price < counterPrice)
                break;
            if (marketData.has_value() && marketData->askPrice > 0 &&
                counterPrice > marketData->askPrice)
                break;
        } else {
            if (counterPrice < order.price)
                break;
            if (marketData.has_value() && marketData->bidPrice > 0 &&
                counterPrice < marketData->bidPrice)
                break;
        }

        // 遍历该价位链表（单遍扫描）
        std::optional<size_t> cur = lvl.head;
        while (cur.has_value() && remainingQty > 0) {
            size_t curIdx = cur.value();
            Order &entry = counterBook.order_at(curIdx);
            std::optional<size_t> nextIdx = entry.next;

            uint32_t matchQty = std::min(remainingQty, entry.remainingQty);

            // B6: 零股处理
            if (order.side == Side::BUY) {
                if (entry.remainingQty >= 100 && matchQty >= 100)
                    matchQty = (matchQty / 100) * 100;
            }
            if (matchQty == 0) {
                cur = nextIdx;
                continue;
            }

            // 构造成交回报
            OrderResponse exec;
            exec.clOrderId = entry.clOrderId;
            exec.market = entry.market;
            exec.securityId = entry.securityId;
            exec.side = entry.side;
            exec.qty = entry.qty;
            exec.price = entry.price;
            exec.shareholderId = entry.shareholderId;
            exec.execId = generateExecId();
            exec.execQty = matchQty;
            exec.execPrice = entry.price; // B4: maker 价
            exec.type = OrderResponse::Type::EXECUTION;
            result.executions.emplace_back(std::move(exec));

            // 更新对手方
            entry.remainingQty -= matchQty;
            entry.cumQty += matchQty;
            remainingQty -= matchQty;

            if (entry.remainingQty == 0) {
                OrderId entryId = entry.clOrderId;
                counterBook.remove(curIdx);
                orderIndex_.remove(entryId);
            }

            cur = nextIdx;
        }

        // 推进到下一个有单的价位（O(1) 跳跃）
        auto nextOpt = counterBook.next_level(lvl_idx);
        if (!nextOpt.has_value()) {
            break;
        }
        lvl_idx = nextOpt.value();
    }

    result.remainingQty = remainingQty;
    return result;
}

// ============================================================
// B7: cancelOrder
// ============================================================
CancelResponse MatchingEngine::cancelOrder(const OrderId &clOrderId) {
    CancelResponse response;
    response.origClOrderId = clOrderId;

    auto *locPtr = orderIndex_.get(clOrderId);
    if (locPtr == nullptr) {
        response.type = CancelResponse::Type::REJECT;
        response.rejectCode = 1;
        response.rejectText = "Order not found in book";
        return response;
    }
    const OrderLocation loc = *locPtr; // copy since we'll remove from index

    auto *sbPtr = books_.get(loc.bookKey);
    if (sbPtr == nullptr) {
        std::cerr << "[MatchingEngine] CRITICAL: book not found for key="
                  << loc.bookKey << "\n";
        response.type = CancelResponse::Type::REJECT;
        response.rejectCode = 2;
        response.rejectText = "Order index inconsistency";
        orderIndex_.remove(clOrderId);
        return response;
    }
    SecurityBook &sb = *sbPtr;
    OrderBook &book =
        (loc.side == Side::BUY) ? sb.bidBook : sb.askBook;

    // O(1) 直接访问 order
    Order &entry = book.order_at(loc.pool_index);

    response.clOrderId = entry.clOrderId;
    response.market = entry.market;
    response.securityId = entry.securityId;
    response.shareholderId = entry.shareholderId;
    response.side = entry.side;
    response.qty = entry.qty;
    response.price = entry.price;
    response.cumQty = entry.cumQty;
    response.canceledQty = entry.remainingQty;
    response.type = CancelResponse::Type::CONFIRM;

    book.remove(loc.pool_index);
    orderIndex_.remove(clOrderId);

    return response;
}

// ============================================================
// B8: reduceOrderQty
// ============================================================
void MatchingEngine::reduceOrderQty(const OrderId &clOrderId, uint32_t qty) {
    auto *locPtr = orderIndex_.get(clOrderId);
    if (locPtr == nullptr)
        return;
    const OrderLocation loc = *locPtr;

    auto *sbPtr = books_.get(loc.bookKey);
    if (sbPtr == nullptr)
        return;
    SecurityBook &sb = *sbPtr;
    OrderBook &book =
        (loc.side == Side::BUY) ? sb.bidBook : sb.askBook;

    Order &entry = book.order_at(loc.pool_index);
    entry.cumQty += qty;

    if (qty >= entry.remainingQty) {
        // 完全消耗
        book.remove(loc.pool_index);
        orderIndex_.remove(clOrderId);
    } else {
        entry.remainingQty -= qty;
        // 更新 PriceLevel 的 total_volume
        size_t lvl_idx = book.price_to_index(entry.price);
        PriceLevel &lvl = book.level_at(lvl_idx);
        lvl.total_volume -= qty;
    }
}

bool MatchingEngine::hasOrder(const OrderId &clOrderId) {
    return orderIndex_.get(clOrderId) != nullptr;
}

// ============================================================
// getSnapshot(securityId, market) — 单证券快照
// ============================================================
nlohmann::json MatchingEngine::getSnapshot(const SecurityId &securityId,
                                           Market market) {
    const BookKey key = makeBookKey(securityId, market);
    auto *sbPtr = books_.get(key);
    if (sbPtr == nullptr) {
        return {{"bids", nlohmann::json::array()},
                {"asks", nlohmann::json::array()},
                {"totalOrders", 0}};
    }
    SecurityBook &sb = *sbPtr;
    int totalOrders = 0;

    // 收集 bid 价位
    nlohmann::json bids = nlohmann::json::array();
    int cumQty = 0;
    auto bidOpt = sb.bidBook.best_level_index();
    while (bidOpt.has_value()) {
        size_t i = bidOpt.value();
        PriceLevel &lvl = sb.bidBook.level_at(i);
        if (lvl.head.has_value()) {
            int qty = 0, cnt = 0;
            std::optional<size_t> cur = lvl.head;
            while (cur.has_value()) {
                Order &o = sb.bidBook.order_at(cur.value());
                qty += (int)o.remainingQty;
                ++cnt;
                cur = o.next;
            }
            totalOrders += cnt;
            cumQty += qty;
            bids.push_back({{"price", sb.bidBook.index_to_price(i)},
                            {"qty", qty},
                            {"cumQty", cumQty},
                            {"orderCount", cnt}});
        }
        bidOpt = sb.bidBook.next_level(i);
    }

    // 收集 ask 价位
    nlohmann::json asks = nlohmann::json::array();
    cumQty = 0;
    auto askOpt = sb.askBook.best_level_index();
    while (askOpt.has_value()) {
        size_t i = askOpt.value();
        PriceLevel &lvl = sb.askBook.level_at(i);
        if (lvl.head.has_value()) {
            int qty = 0, cnt = 0;
            std::optional<size_t> cur = lvl.head;
            while (cur.has_value()) {
                Order &o = sb.askBook.order_at(cur.value());
                qty += (int)o.remainingQty;
                ++cnt;
                cur = o.next;
            }
            totalOrders += cnt;
            cumQty += qty;
            asks.push_back({{"price", sb.askBook.index_to_price(i)},
                            {"qty", qty},
                            {"cumQty", cumQty},
                            {"orderCount", cnt}});
        }
        askOpt = sb.askBook.next_level(i);
    }

    return {{"bids", bids}, {"asks", asks}, {"totalOrders", totalOrders}};
}

// ============================================================
// getSnapshot() — 聚合所有证券快照
// ============================================================
nlohmann::json MatchingEngine::getSnapshot() {
    std::map<double, std::pair<int, int>, std::greater<double>> aggBids;
    std::map<double, std::pair<int, int>> aggAsks;
    int totalOrders = 0;

    auto collectSide = [&](OrderBook &book, auto &aggMap) {
        auto opt = book.best_level_index();
        while (opt.has_value()) {
            size_t i = opt.value();
            PriceLevel &lvl = book.level_at(i);
            if (lvl.head.has_value()) {
                double price = book.index_to_price(i);
                int qty = 0, cnt = 0;
                std::optional<size_t> cur = lvl.head;
                while (cur.has_value()) {
                    Order &o = book.order_at(cur.value());
                    qty += (int)o.remainingQty;
                    ++cnt;
                    cur = o.next;
                }
                aggMap[price].first += qty;
                aggMap[price].second += cnt;
                totalOrders += cnt;
            }
            opt = book.next_level(i);
        }
    };

    books_.for_each([&](const BookKey & /*key*/, SecurityBook &sb) {
        collectSide(sb.bidBook, aggBids);
        collectSide(sb.askBook, aggAsks);
    });

    nlohmann::json bids = nlohmann::json::array();
    int cumQty = 0;
    for (const auto &[price, p] : aggBids) {
        cumQty += p.first;
        bids.push_back({{"price", price},
                        {"qty", p.first},
                        {"cumQty", cumQty},
                        {"orderCount", p.second}});
    }
    nlohmann::json asks = nlohmann::json::array();
    cumQty = 0;
    for (const auto &[price, p] : aggAsks) {
        cumQty += p.first;
        asks.push_back({{"price", price},
                        {"qty", p.first},
                        {"cumQty", cumQty},
                        {"orderCount", p.second}});
    }
    return {{"bids", bids}, {"asks", asks}, {"totalOrders", totalOrders}};
}

// ============================================================
// getBestQuote — O(1) 直接读 best index
// ============================================================
MarketData MatchingEngine::getBestQuote(const SecurityId &securityId,
                                        Market market) {
    MarketData md{0.0, 0.0};
    const BookKey key = makeBookKey(securityId, market);
    auto *sbPtr = books_.get(key);
    if (sbPtr == nullptr)
        return md;
    SecurityBook &sb = *sbPtr;

    auto bidBest = sb.bidBook.best_price();
    if (bidBest.has_value())
        md.bidPrice = bidBest.value();

    auto askBest = sb.askBook.best_price();
    if (askBest.has_value())
        md.askPrice = askBest.value();

    return md;
}

} // namespace hdf
