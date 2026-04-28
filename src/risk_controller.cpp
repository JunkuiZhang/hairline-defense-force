#include "risk_controller.h"

namespace hdf {

RiskController::RiskController() {}

RiskController::~RiskController() {}

RiskController::RiskCheckResult RiskController::checkOrder(const Order &order) {
    if (isCrossTrade(order)) {
        return RiskCheckResult::CROSS_TRADE;
    }
    return RiskCheckResult::PASSED;
}

bool RiskController::isCrossTrade(const Order &order) {
    RiskKey key = makeKey(order.shareholderId, order.market, order.securityId);

    if (order.side == Side::BUY) {
        auto it = sellSide_.find(key);
        if (it != sellSide_.end() && it->second > 0)
            return true;
    } else if (order.side == Side::SELL) {
        auto it = buySide_.find(key);
        if (it != buySide_.end() && it->second > 0)
            return true;
    }
    return false;
}

void RiskController::onOrderAccepted(const Order &order) {
    RiskKey key = makeKey(order.shareholderId, order.market, order.securityId);

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
    auto it = orderMap_.find(origClOrderId);
    if (it == orderMap_.end())
        return;

    const auto &details = it->second;
    RiskKey key = makeKey(details.shareholderId, details.market, details.securityId);

    if (details.side == Side::BUY) {
        auto sit = buySide_.find(key);
        if (sit != buySide_.end()) {
            if (sit->second >= details.remainingQty)
                sit->second -= details.remainingQty;
            else
                sit->second = 0;
            if (sit->second == 0)
                buySide_.erase(sit);
        }
    } else if (details.side == Side::SELL) {
        auto sit = sellSide_.find(key);
        if (sit != sellSide_.end()) {
            if (sit->second >= details.remainingQty)
                sit->second -= details.remainingQty;
            else
                sit->second = 0;
            if (sit->second == 0)
                sellSide_.erase(sit);
        }
    }

    orderMap_.erase(it);
}

void RiskController::onOrderExecuted(const OrderId &clOrderId,
                                     uint32_t execQty) {
    auto it = orderMap_.find(clOrderId);
    if (it == orderMap_.end())
        return;

    auto &details = it->second;
    RiskKey key = makeKey(details.shareholderId, details.market, details.securityId);

    uint32_t reduceQty = std::min(execQty, details.remainingQty);

    if (details.side == Side::BUY) {
        auto sit = buySide_.find(key);
        if (sit != buySide_.end()) {
            if (sit->second >= reduceQty)
                sit->second -= reduceQty;
            else
                sit->second = 0;
            if (sit->second == 0)
                buySide_.erase(sit);
        }
    } else if (details.side == Side::SELL) {
        auto sit = sellSide_.find(key);
        if (sit != sellSide_.end()) {
            if (sit->second >= reduceQty)
                sit->second -= reduceQty;
            else
                sit->second = 0;
            if (sit->second == 0)
                sellSide_.erase(sit);
        }
    }

    details.remainingQty -= reduceQty;
    if (details.remainingQty == 0) {
        orderMap_.erase(it);
    }
}

RiskKey RiskController::makeKey(const ShareholderId &shareholderId,
                                Market market,
                                const SecurityId &securityId) {
    RiskKey key;
    key.append(std::string_view(shareholderId));
    key.append("_");
    char m = '0' + static_cast<char>(market);
    key.append(std::string_view(&m, 1));
    key.append("_");
    key.append(std::string_view(securityId));
    return key;
}

} // namespace hdf
