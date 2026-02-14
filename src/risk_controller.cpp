#include "risk_controller.h"

namespace hdf {

RiskController::RiskController() {}

RiskController::~RiskController() {}

RiskController::RiskCheckResult RiskController::checkOrder(const Order &order) {
    if (isCrossTrade(order)) {
        return RiskCheckResult::CROSS_TRADE;
    } else {
        return RiskCheckResult::PASSED;
    }
}

bool RiskController::isCrossTrade(const Order &order) {
    // 构造组合键：股东号_市场_证券代码
    std::string key =
        makeKey(order.shareholderId, order.market, order.securityId);

    if (order.side == Side::BUY) {
        // 如果是买单，检查卖方表中是否存在该键且数量大于0
        auto it = sellSide_.find(key);
        if (it != sellSide_.end() && it->second > 0) {
            return true; // 存在卖单，判定为对敲
        }
    } else if (order.side == Side::SELL) {
        // 如果是卖单，检查买方表中是否存在该键且数量大于0
        auto it = buySide_.find(key);
        if (it != buySide_.end() && it->second > 0) {
            return true; // 存在买单，判定为对敲
        }
    }

    return false; // 未检测到对敲
}

void RiskController::onOrderAccepted(const Order &order) {
    std::string key =
        makeKey(order.shareholderId, order.market, order.securityId);

    // 记录订单详细信息
    OrderInfo details;
    details.clOrderId = order.clOrderId;
    details.shareholderId = order.shareholderId;
    details.market = order.market;
    details.securityId = order.securityId;
    details.side = order.side;
    details.price = order.price;
    details.remainingQty = order.qty;

    // 添加到订单映射表
    orderMap_[order.clOrderId] = details;

    // 更新买卖方向的聚合数量
    if (order.side == Side::BUY) {
        buySide_[key] += order.qty;
    } else if (order.side == Side::SELL) {
        sellSide_[key] += order.qty;
    }
}

void RiskController::onOrderCanceled(const std::string &origClOrderId) {
    // 查找待撤销订单是否存在
    auto it = orderMap_.find(origClOrderId);
    if (it == orderMap_.end()) {
        return; // 订单不存在，直接返回
    }

    const auto &details = it->second;
    // 构造组合键以查找聚合表
    std::string key =
        makeKey(details.shareholderId, details.market, details.securityId);

    // 根据买卖方向更新聚合数量
    if (details.side == Side::BUY) {
        if (buySide_.count(key)) {
            // 扣减撤销数量
            if (buySide_[key] >= details.remainingQty) {
                buySide_[key] -= details.remainingQty;
            } else {
                buySide_[key] = 0; // 理论上不应发生，防御性编程
            }
            // 如果数量归零，移除该键以节省空间
            if (buySide_[key] == 0) {
                buySide_.erase(key);
            }
        }
    } else if (details.side == Side::SELL) {
        if (sellSide_.count(key)) {
            // 扣减撤销数量
            if (sellSide_[key] >= details.remainingQty) {
                sellSide_[key] -= details.remainingQty;
            } else {
                sellSide_[key] = 0;
            }
            // 如果数量归零，移除该键
            if (sellSide_[key] == 0) {
                sellSide_.erase(key);
            }
        }
    }

    // 从订单详情映射中移除
    orderMap_.erase(it);
}

void RiskController::onOrderExecuted(const std::string &clOrderId,
                                     uint32_t execQty) {
    // 查找发生交易的订单
    auto it = orderMap_.find(clOrderId);
    if (it == orderMap_.end()) {
        return; // 订单不存在，直接返回
    }

    auto &details = it->second;
    std::string key =
        makeKey(details.shareholderId, details.market, details.securityId);

    // 计算实际扣减数量（不超过剩余量）
    uint32_t reduceQty = std::min(execQty, details.remainingQty);

    // 更新聚合表中的数量
    if (details.side == Side::BUY) {
        if (buySide_.count(key)) {
            if (buySide_[key] >= reduceQty) {
                buySide_[key] -= reduceQty;
            } else {
                buySide_[key] = 0;
            }
            if (buySide_[key] == 0) {
                buySide_.erase(key);
            }
        }
    } else if (details.side == Side::SELL) {
        if (sellSide_.count(key)) {
            if (sellSide_[key] >= reduceQty) {
                sellSide_[key] -= reduceQty;
            } else {
                sellSide_[key] = 0;
            }
            if (sellSide_[key] == 0) {
                sellSide_.erase(key);
            }
        }
    }

    // 更新订单自身的剩余数量
    details.remainingQty -= reduceQty;
    // 如果完全成交，则从订单详情映射中移除
    if (details.remainingQty == 0) {
        orderMap_.erase(it);
    }
}

// 辅助函数实现：生成组合键
std::string RiskController::makeKey(const std::string &shareholderId,
                                    Market market,
                                    const std::string &securityId) {
    // 简单拼接，使用下划线分隔，确保唯一性
    // Market 枚举转 int，保证 key 的紧凑和唯一
    return shareholderId + "_" + std::to_string((int)market) + "_" + securityId;
}

} // namespace hdf
