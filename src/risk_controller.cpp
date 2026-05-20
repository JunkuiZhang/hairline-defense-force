#include "risk_controller.h"

namespace hdf {

RiskController::RiskController() {
    buySide_.set_capacity(1000);
    sellSide_.set_capacity(1000);
    orderMap_.set_capacity(1000);
}

RiskController::~RiskController() {}

RiskController::RiskCheckResult
RiskController::checkOrder(const Order &order) {
    if (isCrossTrade(order)) {
        return RiskCheckResult::CROSS_TRADE;
    }
    return RiskCheckResult::PASSED;
}

bool RiskController::isCrossTrade(const Order &order) {
    RiskKey key{order.shareholderId, order.market, order.securityId};

    if (order.side == Side::BUY) {
        auto *v = sellSide_.get(key);
        if (v && *v > 0)
            return true;
    } else if (order.side == Side::SELL) {
        auto *v = buySide_.get(key);
        if (v && *v > 0)
            return true;
    }
    return false;
}

void RiskController::onOrderAccepted(const Order &order) {
    RiskKey key{order.shareholderId, order.market, order.securityId};

    OrderInfo details;
    details.clOrderId = order.clOrderId;
    details.shareholderId = order.shareholderId;
    details.market = order.market;
    details.securityId = order.securityId;
    details.side = order.side;
    details.price = order.price;
    details.remainingQty = order.qty;

    orderMap_[order.clOrderId] = details;

    if (order.side == Side::BUY) {
        buySide_[key] += order.qty;
    } else if (order.side == Side::SELL) {
        sellSide_[key] += order.qty;
    }
}

void RiskController::onOrderCanceled(const OrderId &origClOrderId) {
    auto *detailsPtr = orderMap_.get(origClOrderId);
    if (!detailsPtr)
        return;

    const auto &details = *detailsPtr;
    RiskKey key{details.shareholderId, details.market, details.securityId};

    if (details.side == Side::BUY) {
        auto *v = buySide_.get(key);
        if (v) {
            if (*v >= details.remainingQty)
                *v -= details.remainingQty;
            else
                *v = 0;
            if (*v == 0)
                buySide_.remove(key);
        }
    } else if (details.side == Side::SELL) {
        auto *v = sellSide_.get(key);
        if (v) {
            if (*v >= details.remainingQty)
                *v -= details.remainingQty;
            else
                *v = 0;
            if (*v == 0)
                sellSide_.remove(key);
        }
    }

    orderMap_.remove(origClOrderId);
}

void RiskController::onOrderExecuted(const OrderId &clOrderId,
                                     uint32_t execQty) {
    auto *detailsPtr = orderMap_.get(clOrderId);
    if (!detailsPtr)
        return;

    auto &details = *detailsPtr;
    RiskKey key{details.shareholderId, details.market, details.securityId};

    uint32_t reduceQty = std::min(execQty, details.remainingQty);

    if (details.side == Side::BUY) {
        auto *v = buySide_.get(key);
        if (v) {
            if (*v >= reduceQty)
                *v -= reduceQty;
            else
                *v = 0;
            if (*v == 0)
                buySide_.remove(key);
        }
    } else if (details.side == Side::SELL) {
        auto *v = sellSide_.get(key);
        if (v) {
            if (*v >= reduceQty)
                *v -= reduceQty;
            else
                *v = 0;
            if (*v == 0)
                sellSide_.remove(key);
        }
    }

    details.remainingQty -= reduceQty;
    if (details.remainingQty == 0) {
        orderMap_.remove(clOrderId);
    }
}

} // namespace hdf
