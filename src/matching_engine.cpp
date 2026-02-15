#include "matching_engine.h"
#include "types.h"
#include <iomanip>
#include <sstream>

namespace hdf {

MatchingEngine::MatchingEngine() {}

MatchingEngine::~MatchingEngine() {}

// ============================================================
// B9: execId 生成 — 为每笔成交生成唯一的 execId
// 格式: "EXEC" + 16位数字（左补零），如 "EXEC0000000000000001"
// ============================================================
std::string MatchingEngine::generateExecId() {
    std::ostringstream oss;
    oss << "EXEC" << std::setw(16) << std::setfill('0') << nextExecId_++;
    return oss.str();
}

// ============================================================
// B2: addOrder — 将订单插入订单簿对应位置
// 根据买卖方向插入 bidBook_（买方）或 askBook_（卖方），
// 并建立 orderIndex_ 反向索引以支持快速查找。
// ============================================================
void MatchingEngine::addOrder(const Order &order) {
    BookEntry entry;
    entry.order = order;
    entry.remainingQty = order.qty;
    entry.cumQty = 0;

    if (order.side == Side::BUY) {
        // 买方订单簿：按价格降序（std::greater），同价时间优先（push_back）
        bidBook_[order.price].push_back(entry);
    } else {
        // 卖方订单簿：按价格升序（默认 std::less），同价时间优先（push_back）
        askBook_[order.price].push_back(entry);
    }

    // 建立反向索引，用于 cancelOrder 和 reduceOrderQty 快速定位
    OrderLocation loc;
    loc.price = order.price;
    loc.side = order.side;
    orderIndex_[order.clOrderId] = loc;
}

// ============================================================
// B3-B6: match — 基础撮合 + 成交价算法 + 部分成交 + 零股处理
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
std::optional<MatchingEngine::MatchResult>
MatchingEngine::match(const Order &order,
                      const std::optional<MarketData> &marketData) {

    MatchResult result;
    uint32_t remainingQty = order.qty;

    if (order.side == Side::BUY) {
        // ---- 买单：与卖方订单簿（askBook_）撮合 ----
        // askBook_ 按价格升序排列，最低卖价在最前面

        // 遍历卖方订单簿，从最低价格开始
        auto priceIt = askBook_.begin();
        while (priceIt != askBook_.end() && remainingQty > 0) {
            double askPrice = priceIt->first;

            // 价格不满足：买入价 < 卖出价，无法成交，退出
            if (order.price < askPrice) {
                break;
            }

            // B10（可选）行情约束：如果有行情数据，买价不能高于行情卖价
            if (marketData.has_value()) {
                if (order.price > marketData->askPrice &&
                    marketData->askPrice > 0) {
                    break;
                }
            }

            // 同价格内按时间优先（list 的 front 就是最早挂单）
            auto &priceLevel = priceIt->second;
            auto entryIt = priceLevel.begin();
            while (entryIt != priceLevel.end() && remainingQty > 0) {
                // 确保同一股票
                if (entryIt->order.securityId != order.securityId) {
                    ++entryIt;
                    continue;
                }

                // 计算可成交数量
                uint32_t matchQty =
                    std::min(remainingQty, entryIt->remainingQty);

                // B6: 零股处理
                // 买入单成交数量必须是100的整数倍（除非对手方剩余不足100股）
                // 当对手方（卖方）剩余量不足100时，可以成交零股
                if (entryIt->remainingQty >= 100 && matchQty >= 100) {
                    // 对手方充足，成交量向下取整到100的整数倍
                    matchQty = (matchQty / 100) * 100;
                }
                // 如果对手方剩余量 <
                // 100（零股），则直接全部成交（零股可以不是100的倍数）

                if (matchQty == 0) {
                    ++entryIt;
                    continue;
                }

                // B4: 成交价 = 被动方（maker）挂单价格
                double execPrice = entryIt->order.price;

                // 构建成交回报（对手方 / 被动方信息）
                OrderResponse exec;
                exec.clOrderId = entryIt->order.clOrderId;
                exec.market = entryIt->order.market;
                exec.securityId = entryIt->order.securityId;
                exec.side = entryIt->order.side;
                exec.qty = entryIt->order.qty;     // 原始委托数量
                exec.price = entryIt->order.price; // 原始委托价格
                exec.shareholderId = entryIt->order.shareholderId;
                exec.execId = generateExecId(); // B9: 唯一成交编号
                exec.execQty = matchQty;        // 本笔成交数量
                exec.execPrice = execPrice;     // 成交价格
                exec.type = OrderResponse::Type::EXECUTION;

                result.executions.push_back(exec);

                // 更新对手方订单的剩余量和累计成交量
                entryIt->remainingQty -= matchQty;
                entryIt->cumQty += matchQty;

                // 减少主动方剩余量
                remainingQty -= matchQty;

                // 如果对手方完全成交，从订单簿和索引中移除
                if (entryIt->remainingQty == 0) {
                    orderIndex_.erase(entryIt->order.clOrderId);
                    entryIt = priceLevel.erase(entryIt);
                } else {
                    ++entryIt;
                }
            }

            // 如果该价格档位已无订单，删除该价格层级
            if (priceLevel.empty()) {
                priceIt = askBook_.erase(priceIt);
            } else {
                ++priceIt;
            }
        }
    } else {
        // ---- 卖单：与买方订单簿（bidBook_）撮合 ----
        // bidBook_ 按价格降序排列，最高买价在最前面

        auto priceIt = bidBook_.begin();
        while (priceIt != bidBook_.end() && remainingQty > 0) {
            double bidPrice = priceIt->first;

            // 价格不满足：买入价 < 卖出价，无法成交，退出
            if (bidPrice < order.price) {
                break;
            }

            // B10（可选）行情约束：如果有行情数据，卖价不能低于行情买价
            if (marketData.has_value()) {
                if (order.price < marketData->bidPrice &&
                    marketData->bidPrice > 0) {
                    break;
                }
            }

            auto &priceLevel = priceIt->second;
            auto entryIt = priceLevel.begin();
            while (entryIt != priceLevel.end() && remainingQty > 0) {
                // 确保同一股票
                if (entryIt->order.securityId != order.securityId) {
                    ++entryIt;
                    continue;
                }

                // 计算可成交数量
                uint32_t matchQty =
                    std::min(remainingQty, entryIt->remainingQty);

                // B6: 零股处理
                // 卖出单可以是零股（不需要是100的整数倍）
                // 但买方（对手方 / 被动方）在成交时，如果被动方剩余足够
                // 依然按正常规则成交，零股约束只在下单时验证
                // 此处不需要额外的零股处理逻辑，因为卖单本身可以是零股

                if (matchQty == 0) {
                    ++entryIt;
                    continue;
                }

                // B4: 成交价 = 被动方（maker）挂单价格
                double execPrice = entryIt->order.price;

                // 构建成交回报（对手方 / 被动方信息）
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
                exec.execPrice = execPrice;
                exec.type = OrderResponse::Type::EXECUTION;

                result.executions.push_back(exec);

                // 更新对手方订单的剩余量和累计成交量
                entryIt->remainingQty -= matchQty;
                entryIt->cumQty += matchQty;

                // 减少主动方剩余量
                remainingQty -= matchQty;

                // 如果对手方完全成交，从订单簿和索引中移除
                if (entryIt->remainingQty == 0) {
                    orderIndex_.erase(entryIt->order.clOrderId);
                    entryIt = priceLevel.erase(entryIt);
                } else {
                    ++entryIt;
                }
            }

            // 如果该价格档位已无订单，删除该价格层级
            if (priceLevel.empty()) {
                priceIt = bidBook_.erase(priceIt);
            } else {
                ++priceIt;
            }
        }
    }

    result.remainingQty = remainingQty;

    // 如果没有任何成交，返回 nullopt
    if (result.executions.empty()) {
        return std::nullopt;
    }

    return result;
}

// ============================================================
// B7: cancelOrder — 从订单簿移除指定订单
// 返回 CancelResponse，包含已成交累计量等信息。
// 如果找不到订单，返回拒绝类型的 CancelResponse。
// ============================================================
CancelResponse MatchingEngine::cancelOrder(const std::string &clOrderId) {
    CancelResponse response;
    response.origClOrderId = clOrderId;

    // 通过反向索引快速定位订单
    auto indexIt = orderIndex_.find(clOrderId);
    if (indexIt == orderIndex_.end()) {
        // 订单不在簿中（可能已完全成交或不存在），返回拒绝
        response.type = CancelResponse::Type::REJECT;
        response.rejectCode = 1;
        response.rejectText = "Order not found in book";
        return response;
    }

    OrderLocation loc = indexIt->second;

    if (loc.side == Side::BUY) {
        // 在买方订单簿中查找并移除
        auto priceIt = bidBook_.find(loc.price);
        if (priceIt != bidBook_.end()) {
            auto &priceLevel = priceIt->second;
            for (auto entryIt = priceLevel.begin(); entryIt != priceLevel.end();
                 ++entryIt) {
                if (entryIt->order.clOrderId == clOrderId) {
                    // 填充撤单回报信息
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

                    // 从订单簿中移除
                    priceLevel.erase(entryIt);
                    if (priceLevel.empty()) {
                        bidBook_.erase(priceIt);
                    }
                    // 从反向索引中移除
                    orderIndex_.erase(indexIt);
                    return response;
                }
            }
        }
    } else {
        // 在卖方订单簿中查找并移除
        auto priceIt = askBook_.find(loc.price);
        if (priceIt != askBook_.end()) {
            auto &priceLevel = priceIt->second;
            for (auto entryIt = priceLevel.begin(); entryIt != priceLevel.end();
                 ++entryIt) {
                if (entryIt->order.clOrderId == clOrderId) {
                    // 填充撤单回报信息
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

                    // 从订单簿中移除
                    priceLevel.erase(entryIt);
                    if (priceLevel.empty()) {
                        askBook_.erase(priceIt);
                    }
                    // 从反向索引中移除
                    orderIndex_.erase(indexIt);
                    return response;
                }
            }
        }
    }

    // 理论上不应到此处（索引存在但订单簿中找不到），安全兜底
    response.type = CancelResponse::Type::REJECT;
    response.rejectCode = 2;
    response.rejectText = "Order index inconsistency";
    orderIndex_.erase(indexIt);
    return response;
}

// ============================================================
// B8: reduceOrderQty — 减少订单簿中指定订单的数量
// 用于交易所主动成交后同步内部订单簿状态。
// 若减少后数量为0，则自动从订单簿中移除该订单。
// ============================================================
void MatchingEngine::reduceOrderQty(const std::string &clOrderId,
                                    uint32_t qty) {
    // 通过反向索引快速定位订单
    auto indexIt = orderIndex_.find(clOrderId);
    if (indexIt == orderIndex_.end()) {
        // 订单不在簿中，忽略（可能已经完全成交或被撤单）
        return;
    }

    OrderLocation loc = indexIt->second;

    // 定义查找和更新的 lambda，避免买卖方重复代码
    auto reduceInBook = [&](auto &book) {
        auto priceIt = book.find(loc.price);
        if (priceIt == book.end()) {
            return;
        }
        auto &priceLevel = priceIt->second;
        for (auto entryIt = priceLevel.begin(); entryIt != priceLevel.end();
             ++entryIt) {
            if (entryIt->order.clOrderId == clOrderId) {
                // 更新累计成交量
                entryIt->cumQty += qty;

                // 减少剩余量
                if (qty >= entryIt->remainingQty) {
                    // 完全消耗，从订单簿移除
                    entryIt->remainingQty = 0;
                    priceLevel.erase(entryIt);
                    if (priceLevel.empty()) {
                        book.erase(priceIt);
                    }
                    orderIndex_.erase(indexIt);
                } else {
                    entryIt->remainingQty -= qty;
                }
                return;
            }
        }
    };

    if (loc.side == Side::BUY) {
        reduceInBook(bidBook_);
    } else {
        reduceInBook(askBook_);
    }
}

} // namespace hdf
