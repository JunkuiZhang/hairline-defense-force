#include "matching_engine.h"
#include "types.h"
#include <cstdio>
#include <iostream>
#include <map>

namespace hdf {

MatchingEngine::MatchingEngine() {}
MatchingEngine::~MatchingEngine() {
    for (auto& [key, sb] : books_) {
        for (auto& bucket : sb.bidBook) {
            for (auto it = bucket.level.begin(); it != bucket.level.end(); ++it) {
                it.ptr->~BookEntry();
            }
        }
        for (auto& bucket : sb.askBook) {
            for (auto it = bucket.level.begin(); it != bucket.level.end(); ++it) {
                it.ptr->~BookEntry();
            }
        }
    }
}

// ============================================================
// B9: execId 生成
// ============================================================
ExecId MatchingEngine::generateExecId() {
    const uint64_t currentId = nextExecId_++ % 10000000000000000ULL;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "EXEC%016lu", currentId);
    return ExecId(buf);
}

// ============================================================
// B2: addOrder
// 直接定位该证券的 SecurityBook，无需遍历其他证券。
// ============================================================
void MatchingEngine::addOrder(const Order &order) {
    if (orderIndex_.find(order.clOrderId) != orderIndex_.end())
        return;

    const BookKey bookKey = makeBookKey(order.securityId, order.market);
    SecurityBook &sb = books_[bookKey];

    BookEntry* entry = entryPool_.allocate();
    entry->order = order;
    entry->remainingQty = order.qty;
    entry->cumQty = 0;

    if (order.side == Side::BUY) {
        sb.bidBook[order.price].push_back(entry);
    } else {
        sb.askBook[order.price].push_back(entry);
    }

    // 建立反向索引，用于 cancelOrder 和 reduceOrderQty 快速定位
    orderIndex_[order.clOrderId] = {bookKey, order.price, order.side};
}

// ============================================================
// B3-B6: match
//
// 撮合规则：
//   - 价格优先：买单优先匹配最低卖价，卖单优先匹配最高买价
//   - 时间优先：同价格先挂单先成交
//   - 成交价：以被动方（maker）的挂单价格作为成交价
//   - 部分成交：一笔订单可匹配多个对手方，逐个消耗数量
//   - 零股处理：买入单必须为100股整数倍，卖出单可以不是100股整数倍
//
// 此函数为纯匹配操作：
//   - 不会将新订单入簿
//   - 但会从订单簿中移除/减少已匹配的对手方订单
//   - 返回成交结果和剩余未成交数量
// ============================================================
MatchingEngine::MatchResult
MatchingEngine::match(const Order &order,
                      const std::optional<MarketData> &marketData) {
    MatchResult result;
    uint32_t remainingQty = order.qty;

    const BookKey bookKey = makeBookKey(order.securityId, order.market);
    auto bookIt = books_.find(bookKey);
    if (bookIt == books_.end()) {
        // 该证券暂无订单簿（无对手方），直接返回
        result.remainingQty = remainingQty;
        return result;
    }
    SecurityBook &sb = bookIt->second;

    if (order.side == Side::BUY) {
        // ── 买单：与 askBook 撮合（卖方，价格升序）──
        auto priceIt = sb.askBook.begin();
        while (priceIt != sb.askBook.end() && remainingQty > 0) {
            const uint64_t askPrice = priceIt->price;

            // 价格不满足：买入价 < 卖出价，无法成交，退出
            if (order.price < askPrice)
                break;

            // 行情约束：如果有行情数据，成交价不能高于行情卖价
            if (marketData.has_value() && marketData->askPrice > 0 &&
                askPrice > marketData->askPrice)
                break;

            PriceLevel &level = priceIt->level;
            auto entryIt = level.begin();
            while (entryIt != level.end() && remainingQty > 0) {
                uint32_t matchQty =
                    std::min(remainingQty, entryIt->remainingQty);

                // B6: 零股处理
                if (entryIt->remainingQty >= 100 && matchQty >= 100)
                    matchQty = (matchQty / 100) * 100;
                if (matchQty == 0) {
                    ++entryIt;
                    continue;
                }

                OrderResponse exec;
                exec.clOrderId = entryIt->order.clOrderId;
                exec.market = entryIt->order.market;
                exec.securityId = entryIt->order.securityId;
                exec.side = entryIt->order.side;
                exec.qty = entryIt->order.qty;
                exec.price = entryIt->order.price;
                exec.shareholderId = entryIt->order.shareholderId;
                exec.execId = generateExecId();
                exec.execQty = matchQty;
                exec.execPrice = entryIt->order.price; // B4: maker 价
                exec.type = OrderResponse::Type::EXECUTION;
                result.executions.emplace_back(std::move(exec));

                // 更新对手方订单的剩余量和累计成交量
                entryIt->remainingQty -= matchQty;
                entryIt->cumQty += matchQty;
                remainingQty -= matchQty;

                // 如果对手方完全成交，从订单簿和索引中移除
                if (entryIt->remainingQty == 0) {
                    orderIndex_.erase(entryIt->order.clOrderId);
                    BookEntry* entryToFree = entryIt.ptr;
                    entryIt = level.erase(entryIt);
                    entryPool_.deallocate(entryToFree);
                } else {
                    ++entryIt;
                }
            }
            // 如果该价格档位已无订单，删除该价格层级
            if (level.empty())
                priceIt = sb.askBook.erase(priceIt);
            else
                ++priceIt;
        }
    } else {
        // ── 卖单：与 bidBook 撮合（买方，价格降序）──
        auto priceIt = sb.bidBook.begin();
        while (priceIt != sb.bidBook.end() && remainingQty > 0) {
            const uint64_t bidPrice = priceIt->price;

            // 价格不满足：买入价 < 卖出价，无法成交，退出
            if (bidPrice < order.price)
                break;

            // 行情约束：如果有行情数据，成交价不能低于行情买价
            if (marketData.has_value() && marketData->bidPrice > 0 &&
                bidPrice < marketData->bidPrice)
                break;

            PriceLevel &level = priceIt->level;
            auto entryIt = level.begin();
            while (entryIt != level.end() && remainingQty > 0) {
                uint32_t matchQty =
                    std::min(remainingQty, entryIt->remainingQty);
                if (matchQty == 0) {
                    ++entryIt;
                    continue;
                }

                OrderResponse exec;
                exec.clOrderId = entryIt->order.clOrderId;
                exec.market = entryIt->order.market;
                exec.securityId = entryIt->order.securityId;
                exec.side = entryIt->order.side;
                exec.qty = entryIt->order.qty;
                exec.price = entryIt->order.price;
                exec.shareholderId = entryIt->order.shareholderId;
                exec.execId = generateExecId();
                exec.execQty = matchQty;
                exec.execPrice = entryIt->order.price; // B4: maker 价
                exec.type = OrderResponse::Type::EXECUTION;
                result.executions.emplace_back(std::move(exec));

                // 更新对手方订单的剩余量和累计成交量
                entryIt->remainingQty -= matchQty;
                entryIt->cumQty += matchQty;
                remainingQty -= matchQty;

                // 如果对手方完全成交，从订单簿和索引中移除
                if (entryIt->remainingQty == 0) {
                    orderIndex_.erase(entryIt->order.clOrderId);
                    BookEntry* entryToFree = entryIt.ptr;
                    entryIt = level.erase(entryIt);
                    entryPool_.deallocate(entryToFree);
                } else {
                    ++entryIt;
                }
            }
            // 如果该价格档位已无订单，删除该价格层级
            if (level.empty())
                priceIt = sb.bidBook.erase(priceIt);
            else
                ++priceIt;
        }
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

    auto indexIt = orderIndex_.find(clOrderId);
    if (indexIt == orderIndex_.end()) {
        // 订单不在簿中（可能已完全成交或不存在），返回拒绝
        response.type = CancelResponse::Type::REJECT;
        response.rejectCode = 1;
        response.rejectText = "Order not found in book";
        return response;
    }
    const OrderLocation &loc = indexIt->second;

    auto bookIt = books_.find(loc.bookKey);
    if (bookIt == books_.end()) {
        // 订单簿不存在，说明订单索引不一致，返回拒绝并清理索引
        std::cerr << "[MatchingEngine] CRITICAL: book not found for key="
                  << loc.bookKey << "\n";
        response.type = CancelResponse::Type::REJECT;
        response.rejectCode = 2;
        response.rejectText = "Order index inconsistency";
        orderIndex_.erase(indexIt);
        return response;
    }
    SecurityBook &sb = bookIt->second;

    // 通用取消逻辑（模板 lambda 避免买卖方重复）
    auto doCancel = [&](auto &book) -> bool {
        auto priceIt = book.find(loc.price);
        if (priceIt == book.end())
            return false;
        PriceLevel &level = priceIt->level;
        for (auto entryIt = level.begin(); entryIt != level.end(); ++entryIt) {
            if (entryIt->order.clOrderId != clOrderId)
                continue;
            response.clOrderId = entryIt->order.clOrderId;
            response.market = entryIt->order.market;
            response.securityId = entryIt->order.securityId;
            response.shareholderId = entryIt->order.shareholderId;
            response.side = entryIt->order.side;
            response.qty = entryIt->order.qty;
            response.price = entryIt->order.price;
            response.cumQty = entryIt->cumQty;
            response.canceledQty = entryIt->remainingQty;
            response.type = CancelResponse::Type::CONFIRM;
            BookEntry* entryToFree = entryIt.ptr;
            level.erase(entryIt);
            entryPool_.deallocate(entryToFree);
            if (level.empty())
                book.erase(priceIt);
            orderIndex_.erase(indexIt);
            return true;
        }
        return false;
    };

    bool ok =
        (loc.side == Side::BUY) ? doCancel(sb.bidBook) : doCancel(sb.askBook);
    if (!ok) {
        std::cerr << "[MatchingEngine] CRITICAL: Order index inconsistency for "
                     "clOrderId="
                  << clOrderId << "\n";
        response.type = CancelResponse::Type::REJECT;
        response.rejectCode = 2;
        response.rejectText = "Order index inconsistency";
        orderIndex_.erase(indexIt);
    }
    return response;
}

// ============================================================
// B8: reduceOrderQty
// ============================================================
void MatchingEngine::reduceOrderQty(const OrderId &clOrderId,
                                    uint32_t qty) {
    auto indexIt = orderIndex_.find(clOrderId);
    if (indexIt == orderIndex_.end())
        // 订单不在簿中，忽略（可能已经完全成交或被撤单）
        return;

    const OrderLocation &loc = indexIt->second;
    auto bookIt = books_.find(loc.bookKey);
    if (bookIt == books_.end())
        return;
    SecurityBook &sb = bookIt->second;

    auto reduceInBook = [&](auto &book) {
        auto priceIt = book.find(loc.price);
        if (priceIt == book.end())
            return;
        PriceLevel &level = priceIt->level;
        for (auto entryIt = level.begin(); entryIt != level.end(); ++entryIt) {
            if (entryIt->order.clOrderId != clOrderId)
                continue;
            // 更新累计成交量
            entryIt->cumQty += qty;
            // 减少剩余量
            if (qty >= entryIt->remainingQty) {
                // 完全消耗，从订单簿移除
                entryIt->remainingQty = 0;
                BookEntry* entryToFree = entryIt.ptr;
                level.erase(entryIt);
                entryPool_.deallocate(entryToFree);
                if (level.empty())
                    book.erase(priceIt);
                orderIndex_.erase(indexIt);
            } else {
                entryIt->remainingQty -= qty;
            }
            return;
        }
    };

    if (loc.side == Side::BUY)
        reduceInBook(sb.bidBook);
    else
        reduceInBook(sb.askBook);
}

bool MatchingEngine::hasOrder(const OrderId &clOrderId) const {
    return orderIndex_.count(clOrderId) > 0;
}

// ============================================================
// getSnapshot(securityId, market) — 单证券快照
// ============================================================
nlohmann::json MatchingEngine::getSnapshot(const SecurityId &securityId,
                                           Market market) const {
    const BookKey key = makeBookKey(securityId, market);
    auto bookIt = books_.find(key);
    if (bookIt == books_.end()) {
        return {{"bids", nlohmann::json::array()},
                {"asks", nlohmann::json::array()},
                {"totalOrders", 0}};
    }
    const SecurityBook &sb = bookIt->second;

    nlohmann::json bids = nlohmann::json::array();
    int cumQty = 0, totalOrders = 0;
    for (const auto &bucket : sb.bidBook) {
        int qty = 0, cnt = 0;
        for (const auto &e : bucket.level) {
            qty += (int)e.remainingQty;
            ++cnt;
            ++totalOrders;
        }
        cumQty += qty;
        bids.push_back({{"price", static_cast<double>(bucket.price) / 10000.0},
                        {"qty", qty},
                        {"cumQty", cumQty},
                        {"orderCount", cnt}});
    }
    nlohmann::json asks = nlohmann::json::array();
    cumQty = 0;
    for (const auto &bucket : sb.askBook) {
        int qty = 0, cnt = 0;
        for (const auto &e : bucket.level) {
            qty += (int)e.remainingQty;
            ++cnt;
            ++totalOrders;
        }
        cumQty += qty;
        asks.push_back({{"price", static_cast<double>(bucket.price) / 10000.0},
                        {"qty", qty},
                        {"cumQty", cumQty},
                        {"orderCount", cnt}});
    }
    return {{"bids", bids}, {"asks", asks}, {"totalOrders", totalOrders}};
}

// ============================================================
// getSnapshot() — 聚合所有证券快照
// 仅在单证券或调试场景使用；多证券场景请用 getSnapshot(id, market)。
// ============================================================
nlohmann::json MatchingEngine::getSnapshot() const {
    // price -> {qty, orderCount}
    std::map<uint64_t, std::pair<int, int>, std::greater<uint64_t>> aggBids;
    std::map<uint64_t, std::pair<int, int>> aggAsks;
    int totalOrders = 0;

    for (const auto &[key, sb] : books_) {
        for (const auto &bucket : sb.bidBook)
            for (const auto &e : bucket.level) {
                aggBids[bucket.price].first += (int)e.remainingQty;
                aggBids[bucket.price].second += 1;
                ++totalOrders;
            }
        for (const auto &bucket : sb.askBook)
            for (const auto &e : bucket.level) {
                aggAsks[bucket.price].first += (int)e.remainingQty;
                aggAsks[bucket.price].second += 1;
                ++totalOrders;
            }
    }

    nlohmann::json bids = nlohmann::json::array();
    int cumQty = 0;
    for (const auto &[price, p] : aggBids) {
        cumQty += p.first;
        bids.push_back({{"price", static_cast<double>(price) / 10000.0},
                        {"qty", p.first},
                        {"cumQty", cumQty},
                        {"orderCount", p.second}});
    }
    nlohmann::json asks = nlohmann::json::array();
    cumQty = 0;
    for (const auto &[price, p] : aggAsks) {
        cumQty += p.first;
        asks.push_back({{"price", static_cast<double>(price) / 10000.0},
                        {"qty", p.first},
                        {"cumQty", cumQty},
                        {"orderCount", p.second}});
    }
    return {{"bids", bids}, {"asks", asks}, {"totalOrders", totalOrders}};
}

// ============================================================
// getBestQuote — O(1) 直接定位 SecurityBook
// ============================================================
MarketData MatchingEngine::getBestQuote(const SecurityId &securityId,
                                        Market market) const {
    MarketData md{0, 0};
    const BookKey key = makeBookKey(securityId, market);
    auto bookIt = books_.find(key);
    if (bookIt == books_.end())
        return md;
    const SecurityBook &sb = bookIt->second;

    // bidBook 降序，front 即为最优买价（空层级已在撮合时清除）
    if (!sb.bidBook.empty())
        md.bidPrice = sb.bidBook.front().price;

    // askBook 升序，front 即为最优卖价
    if (!sb.askBook.empty())
        md.askPrice = sb.askBook.front().price;

    return md;
}

} // namespace hdf
