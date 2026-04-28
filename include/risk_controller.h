#pragma once

#include "types.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace hdf {

class RiskController {
  public:
    enum class RiskCheckResult {
        PASSED,      // 风控检查通过
        CROSS_TRADE, // 检测到对敲风险
    };

    RiskController();
    ~RiskController();

    RiskCheckResult checkOrder(const Order &order);
    bool isCrossTrade(const Order &order);
    void onOrderAccepted(const Order &order);
    void onOrderCanceled(const OrderId &origClOrderId);
    void onOrderExecuted(const OrderId &clOrderId, uint32_t execQty);

  private:
    struct OrderInfo {
        OrderId clOrderId;
        ShareholderId shareholderId;
        Market market;
        SecurityId securityId;
        Side side;
        double price;
        uint32_t remainingQty;
    };

    RiskKey makeKey(const ShareholderId &shareholderId, Market market,
                    const SecurityId &securityId);

    std::unordered_map<RiskKey, uint32_t> buySide_;
    std::unordered_map<RiskKey, uint32_t> sellSide_;

    std::unordered_map<OrderId, OrderInfo> orderMap_;
};

} // namespace hdf
