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
    // TODO: 实现对敲检测逻辑
    return false;
}

void RiskController::onOrderAccepted(const Order &order) {
    // TODO: 跟踪订单状态以实现对敲检测
}

void RiskController::onOrderCanceled(const std::string &origClOrderId) {
    // TODO: 跟踪订单状态以实现对敲检测
}

void RiskController::onOrderExecuted(const std::string &clOrderId,
                                     uint32_t execQty) {
    // TODO: 跟踪订单状态以实现对敲检测
}

} // namespace hdf
