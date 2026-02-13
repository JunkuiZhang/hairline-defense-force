#pragma once

#include "types.h"
#include <unordered_map>
#include <string>
#include <vector>

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
    struct OrderInfo {
        std::string clOrderId;
        std::string securityId;
        Side side;
        double price;
        uint32_t remainingQty;
    };

    using SideOrders = std::unordered_map<Side, std::vector<OrderInfo>>;
    using SecurityOrders = std::unordered_map<std::string, SideOrders>;
    using ShareholderOrders = std::unordered_map<std::string, SecurityOrders>;

    ShareholderOrders activeOrders_;
};

} // namespace hdf
