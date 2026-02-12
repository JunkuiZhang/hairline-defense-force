#include "risk_controller.h"
#include "types.h"
#include <gtest/gtest.h>

using namespace hdf;

class RiskControllerTest : public ::testing::Test {
  protected:
    RiskController riskController;
};

TEST_F(RiskControllerTest, CrossTradeDetection) {
    Order buyOrder;
    buyOrder.clOrderId = "1001";
    buyOrder.market = Market::XSHG;
    buyOrder.securityId = "600000";
    buyOrder.side = Side::BUY;
    buyOrder.price = 10.0;
    buyOrder.qty = 1000;
    buyOrder.shareholderId = "SH001";

    // 目前订单簿为空，因此不应该检测到对敲
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder);

    Order sellOrder;
    sellOrder.clOrderId = "1002";
    sellOrder.market = Market::XSHG;
    sellOrder.securityId = "600000";
    sellOrder.side = Side::SELL;
    sellOrder.price = 9.0;
    sellOrder.qty = 500;
    sellOrder.shareholderId = "SH002"; // 不同股东id，非对敲

    // 应该检测到对敲
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::PASSED);

    sellOrder.shareholderId = "SH001"; // 同一股东id，构成对敲
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);
}
