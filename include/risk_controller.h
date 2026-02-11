#pragma once

#include "types.h"

namespace hdf {

class RiskController {
  public:
    enum class RiskCheckResult {
        PASSED,
        CROSS_TRADE,
    };

    RiskController();
    ~RiskController();

    /**
     * @brief 检查订单是否符合风控要求。
     *
     * @param order 要检查的订单。
     * @return RiskCheckResult
     */
    RiskCheckResult checkOrder(const Order &order);

    /**
     * @brief 检查订单是否会导致对敲交易。
     */
    bool isCrossTrade(const Order &order);

    void onOrderAccepted(const Order &order);
    void onOrderCanceled(const std::string &origClOrderId);
    void onOrderExecuted(const std::string &clOrderId, uint32_t execQty);

  private:
    // 为了实现对敲检测而维护的内部状态
};

} // namespace hdf
