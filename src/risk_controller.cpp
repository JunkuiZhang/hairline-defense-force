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
    auto shareholderIt = activeOrders_.find(order.shareholderId);
    if (shareholderIt == activeOrders_.end()) {
        return false;
    }

    auto securityIt = shareholderIt->second.find(order.securityId);
    if (securityIt == shareholderIt->second.end()) {
        return false;
    }

    Side oppositeSide = (order.side == Side::BUY) ? Side::SELL : Side::BUY;
    auto oppositeIt = securityIt->second.find(oppositeSide);
    if (oppositeIt == securityIt->second.end()) {
        return false;
    }

    for (const auto &orderInfo : oppositeIt->second) {
        if (orderInfo.remainingQty > 0) {
            return true;
        }
    }

    return false;
}

void RiskController::onOrderAccepted(const Order &order) {
    OrderInfo orderInfo;
    orderInfo.clOrderId = order.clOrderId;
    orderInfo.securityId = order.securityId;
    orderInfo.side = order.side;
    orderInfo.price = order.price;
    orderInfo.remainingQty = order.qty;

    activeOrders_[order.shareholderId][order.securityId][order.side].push_back(orderInfo);
}

void RiskController::onOrderCanceled(const std::string &origClOrderId) {
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
    for (auto &shareholderPair : activeOrders_) {
        for (auto &securityPair : shareholderPair.second) {
            for (auto &sidePair : securityPair.second) {
                for (auto &orderInfo : sidePair.second) {
                    if (orderInfo.clOrderId == clOrderId) {
                        if (execQty >= orderInfo.remainingQty) {
                            orderInfo.remainingQty = 0;
                        } else {
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
