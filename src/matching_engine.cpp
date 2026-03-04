#include "matching_engine.h"
#include "types.h"
#include <format>
#include <iostream>

namespace hdf {

MatchingEngine::MatchingEngine() {}
MatchingEngine::~MatchingEngine() {}

// ============================================================
// B9: execId 生成
// ============================================================
std::string MatchingEngine::generateExecId() {
    const uint64_t currentId = nextExecId_++ % 10000000000000000ULL;
    return std::format("EXEC{:016}", currentId);
}

// ============================================================
// B2: addOrder
// 直接定位该证券的 SecurityBook，无需遍历其他证券。
// ============================================================
void MatchingEngine::addOrder(const Order &order) {
    if (orderIndex_.find(order.clOrderId) != orderIndex_.end())
        return;

    const std::string bookKey = makeBookKey(order.securityId, order.market);
    SecurityBook &sb = books_[bookKey];

    BookEntry entry;
    entry.order = order;
    entry.remainingQty = order.qty;
    entry.cumQty = 0;

    if (order.side == Side::BUY) {
        sb.bidBook[order.price].push_back(entry);
    } else {
        sb.askBook[order.price].push_back(entry);
    }

    orderIndex_[order.clOrderId] = {bookKey, order.price, order.side};
}

// ============================================================
// B3-B6: match
//
// 直接通过 makeBookKey 定位目标 SecurityBook，
// 无需在循环内比较 securityId/market，彻底消除原实现中的
// 约38% 无效字符串比较开销。
// ============================================================
MatchingEngine::MatchResult
MatchingEngine::match(const Order &order,
                      const std::optional<MarketData> &marketData) {
    MatchResult result;
    uint32_t remainingQty = order.qty;

    const std::string bookKey = makeBookKey(order.securityId, order.market);
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
            const double askPrice = priceIt->first;

            if (order.price < askPrice)
                break;
            if (marketData.has_value() && marketData->askPrice > 0 &&
                askPrice > marketData->askPrice)
                break;

            PriceLevel &level = priceIt->second;
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

                entryIt->remainingQty -= matchQty;
                entryIt->cumQty += matchQty;
                remainingQty -= matchQty;

                if (entryIt->remainingQty == 0) {
                    orderIndex_.erase(entryIt->order.clOrderId);
                    entryIt = level.erase(entryIt);
                } else {
                    ++entryIt;
                }
            }
            if (level.empty())
                priceIt = sb.askBook.erase(priceIt);
            else
                ++priceIt;
        }
    } else {
        // ── 卖单：与 bidBook 撮合（买方，价格降序）──
        auto priceIt = sb.bidBook.begin();
        while (priceIt != sb.bidBook.end() && remainingQty > 0) {
            const double bidPrice = priceIt->first;

            if (bidPrice < order.price)
                break;
            if (marketData.has_value() && marketData->bidPrice > 0 &&
                bidPrice < marketData->bidPrice)
                break;

            PriceLevel &level = priceIt->second;
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

                entryIt->remainingQty -= matchQty;
                entryIt->cumQty += matchQty;
                remainingQty -= matchQty;

                if (entryIt->remainingQty == 0) {
                    orderIndex_.erase(entryIt->order.clOrderId);
                    entryIt = level.erase(entryIt);
                } else {
                    ++entryIt;
                }
            }
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
CancelResponse MatchingEngine::cancelOrder(const std::string &clOrderId) {
    CancelResponse response;
    response.origClOrderId = clOrderId;

    auto indexIt = orderIndex_.find(clOrderId);
    if (indexIt == orderIndex_.end()) {
        response.type = CancelResponse::Type::REJECT;
        response.rejectCode = 1;
        response.rejectText = "Order not found in book";
        return response;
    }
    const OrderLocation &loc = indexIt->second;

    auto bookIt = books_.find(loc.bookKey);
    if (bookIt == books_.end()) {
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
        PriceLevel &level = priceIt->second;
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
            level.erase(entryIt);
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
void MatchingEngine::reduceOrderQty(const std::string &clOrderId,
                                    uint32_t qty) {
    auto indexIt = orderIndex_.find(clOrderId);
    if (indexIt == orderIndex_.end())
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
        PriceLevel &level = priceIt->second;
        for (auto entryIt = level.begin(); entryIt != level.end(); ++entryIt) {
            if (entryIt->order.clOrderId != clOrderId)
                continue;
            entryIt->cumQty += qty;
            if (qty >= entryIt->remainingQty) {
                entryIt->remainingQty = 0;
                level.erase(entryIt);
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

bool MatchingEngine::hasOrder(const std::string &clOrderId) const {
    return orderIndex_.count(clOrderId) > 0;
}

// ============================================================
// getSnapshot(securityId, market) — 单证券快照
// ============================================================
nlohmann::json MatchingEngine::getSnapshot(const std::string &securityId,
                                           Market market) const {
    const std::string key = makeBookKey(securityId, market);
    auto bookIt = books_.find(key);
    if (bookIt == books_.end()) {
        return {{"bids", nlohmann::json::array()},
                {"asks", nlohmann::json::array()},
                {"totalOrders", 0}};
    }
    const SecurityBook &sb = bookIt->second;

    nlohmann::json bids = nlohmann::json::array();
    int cumQty = 0, totalOrders = 0;
    for (const auto &[price, level] : sb.bidBook) {
        int qty = 0, cnt = 0;
        for (const auto &e : level) {
            qty += (int)e.remainingQty;
            ++cnt;
            ++totalOrders;
        }
        cumQty += qty;
        bids.push_back({{"price", price},
                        {"qty", qty},
                        {"cumQty", cumQty},
                        {"orderCount", cnt}});
    }
    nlohmann::json asks = nlohmann::json::array();
    cumQty = 0;
    for (const auto &[price, level] : sb.askBook) {
        int qty = 0, cnt = 0;
        for (const auto &e : level) {
            qty += (int)e.remainingQty;
            ++cnt;
            ++totalOrders;
        }
        cumQty += qty;
        asks.push_back({{"price", price},
                        {"qty", qty},
                        {"cumQty", cumQty},
                        {"orderCount", cnt}});
    }
    return {{"bids", bids}, {"asks", asks}, {"totalOrders", totalOrders}};
}

// ============================================================
// getSnapshot() — 聚合所有证券快照（向后兼容）
// 仅在单证券或调试场景使用；多证券场景请用 getSnapshot(id, market)。
// ============================================================
nlohmann::json MatchingEngine::getSnapshot() const {
    // price -> {qty, orderCount}
    std::map<double, std::pair<int, int>, std::greater<double>> aggBids;
    std::map<double, std::pair<int, int>> aggAsks;
    int totalOrders = 0;

    for (const auto &[key, sb] : books_) {
        for (const auto &[price, level] : sb.bidBook)
            for (const auto &e : level) {
                aggBids[price].first += (int)e.remainingQty;
                aggBids[price].second += 1;
                ++totalOrders;
            }
        for (const auto &[price, level] : sb.askBook)
            for (const auto &e : level) {
                aggAsks[price].first += (int)e.remainingQty;
                aggAsks[price].second += 1;
                ++totalOrders;
            }
    }

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
// getBestQuote — O(1) 直接定位 SecurityBook
// ============================================================
MarketData MatchingEngine::getBestQuote(const std::string &securityId,
                                        Market market) const {
    MarketData md{0.0, 0.0};
    const std::string key = makeBookKey(securityId, market);
    auto bookIt = books_.find(key);
    if (bookIt == books_.end())
        return md;
    const SecurityBook &sb = bookIt->second;

    // bidBook 降序，首个有剩余的条目即最优买价
    for (const auto &[price, level] : sb.bidBook) {
        for (const auto &entry : level) {
            if (entry.remainingQty > 0) {
                md.bidPrice = price;
                goto foundBid;
            }
        }
    }
foundBid:
    // askBook 升序，首个有剩余的条目即最优卖价
    for (const auto &[price, level] : sb.askBook) {
        for (const auto &entry : level) {
            if (entry.remainingQty > 0) {
                md.askPrice = price;
                goto foundAsk;
            }
        }
    }
foundAsk:
    return md;
}

} // namespace hdf
