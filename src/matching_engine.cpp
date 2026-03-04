#include "matching_engine.h"
#include "types.h"
#include <algorithm>
#include <iostream>
#include <sstream>

namespace hdf {

MatchingEngine::MatchingEngine() {}

MatchingEngine::~MatchingEngine() {}

std::string MatchingEngine::makeBookKey(const std::string& market, const std::string& securityId) {
    return to_string(market) + ":" + securityId;
}

void MatchingEngine::addOrder(const Order &order) {
    std::string key = makeBookKey(to_string(order.market), order.securityId);
    SecurityOrderBook& book = books_[key];

    auto& targetMap = (order.side == Side::BUY) ? book.bids : book.asks;

    // 插入到对应价格档位的列表末尾 (时间优先)
    targetMap[order.price].push_back(order);
    
    // 获取刚插入元素的迭代器
    auto& orderList = targetMap[order.price];
    auto it = std::prev(orderList.end());
    
    // 更新局部索引
    book.orderIndex[order.clOrderId] = {order.side, order.price, it};

    // 更新全局索引 (关键优化)
    globalOrderIndex_[order.clOrderId] = {key, order.side, order.price, it};
}

void MatchingEngine::removeOrderFromIndex(const std::string& clOrderId, const std::string& bookKey) {
    // 从全局索引移除
    globalOrderIndex_.erase(clOrderId);

    // 从局部索引移除
    auto bookIt = books_.find(bookKey);
    if (bookIt != books_.end()) {
        bookIt->second.orderIndex.erase(clOrderId);
    }
}

void MatchingEngine::executeMatch(const Order& incomingOrder, 
                                  SecurityOrderBook& book, 
                                  const std::string& bookKey,
                                  MatchResult& result) {
    
    uint32_t remainingQty = incomingOrder.qty;
    bool isBuy = (incomingOrder.side == Side::BUY);
    auto& opponentMap = isBuy ? book.asks : book.bids;

    // 浮点数比较容差 (虽然 map key 是 exact match，但在逻辑判断价格优劣时需小心)
    // 这里主要依赖 map 的有序性。
    // 买入：找 ask <= buyPrice
    // 卖出：找 bid >= sellPrice
    
    auto it = isBuy ? opponentMap.begin() : opponentMap.rbegin();
    auto endIt = isBuy ? opponentMap.end() : opponentMap.rend();

    while (remainingQty > 0 && it != endIt) {
        double opponentPrice = it->first;
        
        // 价格检查
        if (isBuy) {
            if (opponentPrice > incomingOrder.price + 1e-9) break; // 卖价太高
        } else {
            if (opponentPrice < incomingOrder.price - 1e-9) break; // 买价太低
        }

        auto& orderList = it->second;
        
        // 遍历该价位的所有订单
        while (remainingQty > 0 && !orderList.empty()) {
            Order& opponentOrder = orderList.front();
            
            uint32_t matchQty = std::min(remainingQty, opponentOrder.qty);
            
            // 构建成交回报
            OrderResponse response;
            response.clOrderId = opponentOrder.clOrderId;
            response.market = opponentOrder.market;
            response.securityId = opponentOrder.securityId;
            response.side = opponentOrder.side;
            response.qty = opponentOrder.qty; // 原订单数量
            response.price = opponentPrice;
            response.shareholderId = opponentOrder.shareholderId;
            
            response.type = OrderResponse::EXECUTION;
            response.execQty = matchQty;
            response.execPrice = opponentPrice;
            response.execId = "EXEC_" + opponentOrder.clOrderId + "_" + incomingOrder.clOrderId; // 简单生成ID
            
            result.executions.push_back(response);

            remainingQty -= matchQty;
            opponentOrder.qty -= matchQty;

            std::string oppClOrderId = opponentOrder.clOrderId;

            if (opponentOrder.qty == 0) {
                // 完全成交：移除
                orderList.pop_front();
                removeOrderFromIndex(oppClOrderId, bookKey);
            } else {
                // 部分成交：更新全局索引中的迭代器指向 (其实迭代器没变，只是对象内容变了)
                // 不需要额外操作，因为 globalOrderIndex_ 存的是 iterator，指向的对象已修改
                break; 
            }
        }

        // 清理空的价格档位
        if (orderList.empty()) {
            if (!isBuy) {
                // 反向迭代器 erase 处理
                auto current_it = it;
                ++it; 
                opponentMap.erase(std::next(current_it).base());
            } else {
                it = opponentMap.erase(it);
            }
        } else {
            ++it;
        }
    }

    result.remainingQty = remainingQty;
}

std::optional<MatchingEngine::MatchResult>
MatchingEngine::match(const Order &order,
                      const std::optional<MarketData> &marketData) {
    
    std::string key = makeBookKey(to_string(order.market), order.securityId);
    auto it = books_.find(key);
    
    MatchResult result;
    result.remainingQty = order.qty;

    if (it != books_.end()) {
        executeMatch(order, it->second, key, result);
    }

    // 即使没有成交 (remainingQty == order.qty)，也返回结果让调用方知道状态
    // 如果完全没撮合到，executions 为空，remainingQty 等于原数量
    return result;
}

CancelResponse MatchingEngine::cancelOrder(const std::string &clOrderId) {
    CancelResponse response;
    response.origClOrderId = clOrderId;
    response.canceledQty = 0;
    response.type = CancelResponse::REJECT;
    response.rejectCode = -1;
    response.rejectText = "Order not found";

    // O(1) 查找：利用全局索引
    auto globalIt = globalOrderIndex_.find(clOrderId);
    if (globalIt == globalOrderIndex_.end()) {
        return response;
    }

    const GlobalLoc& loc = globalIt->second;
    
    // 定位到具体的 book
    auto bookIt = books_.find(loc.bookKey);
    if (bookIt == books_.end()) {
        // 数据不一致，理论上不应发生
        return response;
    }

    SecurityOrderBook& book = bookIt->second;
    auto& targetMap = (loc.side == Side::BUY) ? book.bids : book.asks;
    
    // 验证价格档位是否存在 (防御性编程)
    auto priceIt = targetMap.find(loc.price);
    if (priceIt == targetMap.end()) {
        return response;
    }

    Order& targetOrder = *(loc.it);
    
    // 填充成功响应
    response.type = CancelResponse::CONFIRM;
    response.rejectCode = 0;
    response.rejectText = "";
    response.market = targetOrder.market;
    response.securityId = targetOrder.securityId;
    response.side = targetOrder.side;
    response.qty = targetOrder.qty;
    response.price = targetOrder.price;
    response.cumQty = 0; // 简化处理，实际需记录累计成交
    response.canceledQty = targetOrder.qty;

    // 执行删除
    priceIt->second.erase(loc.it);
    if (priceIt->second.empty()) {
        targetMap.erase(priceIt);
    }

    // 清除索引
    removeOrderFromIndex(clOrderId, loc.bookKey);

    return response;
}

void MatchingEngine::reduceOrderQty(const std::string &clOrderId, uint32_t qty) {
    auto globalIt = globalOrderIndex_.find(clOrderId);
    if (globalIt == globalOrderIndex_.end()) {
        return; // 未找到
    }

    const GlobalLoc& loc = globalIt->second;
    auto bookIt = books_.find(loc.bookKey);
    if (bookIt == books_.end()) return;

    SecurityOrderBook& book = bookIt->second;
    auto& targetMap = (loc.side == Side::BUY) ? book.bids : book.asks;
    
    auto priceIt = targetMap.find(loc.price);
    if (priceIt == targetMap.end()) return;

    Order& targetOrder = *(loc.it);

    if (targetOrder.qty <= qty) {
        // 减少后为0，相当于撤单
        priceIt->second.erase(loc.it);
        if (priceIt->second.empty()) {
            targetMap.erase(priceIt);
        }
        removeOrderFromIndex(clOrderId, loc.bookKey);
    } else {
        targetOrder.qty -= qty;
        // 数量变化不影响迭代器位置，索引无需更新
    }
}

} // namespace hdf