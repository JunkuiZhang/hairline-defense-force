#pragma once

#include "matching_engine.h"
#include "risk_controller.h"
#include <functional>
#include <nlohmann/json.hpp>

namespace hdf {

class TradeSystem {
  public:
    TradeSystem();
    ~TradeSystem();

    using OrderCallback = std::function<void(const nlohmann::json &)>;
    using ResponseCallback = std::function<void(const nlohmann::json &)>;

    /**
     * @brief 设置订单处理结果的回调函数（例如输出到stdout).
     */
    void setOrderCallback(OrderCallback callback);
    /**
     * @brief 设置响应处理结果的回调函数（例如输出到stdout).
     */
    void setResponseCallback(ResponseCallback callback);

    void handleOrder(const nlohmann::json &input);
    void handleCancel(const nlohmann::json &input);
    void handleMarketData(const nlohmann::json &input);
    void handleResponse(const nlohmann::json &input);

  private:
    RiskController riskController_;
    MatchingEngine matchingEngine_;
    OrderCallback orderCallback_;
    ResponseCallback responseCallback_;
};

} // namespace hdf
