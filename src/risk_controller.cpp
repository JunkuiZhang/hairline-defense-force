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
    // 第一步：查找该股东号是否存在活跃订单
    auto shareholderIt = activeOrders_.find(order.shareholderId);
    if (shareholderIt == activeOrders_.end()) {
        return false;
    }

    // 第二步：查找该股票是否存在活跃订单
    auto securityIt = shareholderIt->second.find(order.securityId);
    if (securityIt == shareholderIt->second.end()) {
        return false;
    }

    // 第三步：确定反方向（买单查卖单，卖单查买单）
    Side oppositeSide = (order.side == Side::BUY) ? Side::SELL : Side::BUY;
    auto oppositeIt = securityIt->second.find(oppositeSide);
    if (oppositeIt == securityIt->second.end()) {
        return false;
    }

    // 第四步：检查反方向订单是否有剩余数量
    // 只有剩余数量大于0的订单才构成对敲风险
    for (const auto &orderInfo : oppositeIt->second) {
        if (orderInfo.remainingQty > 0) {
            return true;
        }
    }

    return false;
}

void RiskController::onOrderAccepted(const Order &order) {
    // 创建订单信息对象
    OrderInfo orderInfo;
    orderInfo.clOrderId = order.clOrderId;
    orderInfo.securityId = order.securityId;
    orderInfo.side = order.side;
    orderInfo.price = order.price;
    orderInfo.remainingQty = order.qty;

    // 将订单添加到三层索引结构中
    // 路径：股东号 -> 股票代码 -> 买卖方向 -> 订单列表
    activeOrders_[order.shareholderId][order.securityId][order.side].push_back(orderInfo);
}

void RiskController::onOrderCanceled(const std::string &origClOrderId) {
    // 遍历三层索引结构，查找并删除指定订单
    for (auto &shareholderPair : activeOrders_) {
        for (auto &securityPair : shareholderPair.second) {
            for (auto &sidePair : securityPair.second) {
                auto &orders = sidePair.second;
                for (auto it = orders.begin(); it != orders.end(); ++it) {
                    if (it->clOrderId == origClOrderId) {
                        orders.erase(it);
                        return;
                    }
                }
            }
        }
    }
}

void RiskController::onOrderExecuted(const std::string &clOrderId,
                                     uint32_t execQty) {
    // 遍历三层索引结构，查找并更新指定订单的剩余数量
    for (auto &shareholderPair : activeOrders_) {
        for (auto &securityPair : shareholderPair.second) {
            for (auto &sidePair : securityPair.second) {
                for (auto &orderInfo : sidePair.second) {
                    if (orderInfo.clOrderId == clOrderId) {
                        // 更新剩余数量
                        if (execQty >= orderInfo.remainingQty) {
                            // 完全成交，剩余数量设为0
                            orderInfo.remainingQty = 0;
                        } else {
                            // 部分成交，减少剩余数量
                            orderInfo.remainingQty -= execQty;
                        }
                        return;
                    }
                }
            }
        }
    }
}

} // namespace hdf
