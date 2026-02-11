#include "risk_controller.h"
#include "types.h"
#include <gtest/gtest.h>

using namespace hdf;

class RiskControllerTest : public ::testing::Test {
  protected:
    RiskController riskController;
};

TEST_F(RiskControllerTest, BasicOrderValidation) {
    Order order;
    order.clOrderId = "1001";
    order.market = Market::XSHG;
    order.securityId = "600000";
    order.side = Side::BUY;
    order.price = 10.0;
    order.qty = 100;
    order.shareholderId = "SH001";

    // Assuming default implementation returns PASSED for now,
    // or checks basic validity (Price > 0, Qty > 0).
    // Adjust expectations based on what you plan to implement.
    EXPECT_EQ(riskController.checkOrder(order),
              RiskController::RiskCheckResult::PASSED);

    // Invalid Price
    order.price = -1.0;
    // EXPECT_EQ(riskController.checkOrder(order),
    // RiskController::RiskCheckResult::REJECTED); // Uncomment when implemented
}

TEST_F(RiskControllerTest, CrossTradeDetection) {
    // Setup: Simulate an existing order in the system (if RiskController tracks
    // state) Or if RiskController is stateless regarding the book, this test
    // might need MatchingEngine context. However, the interface has
    // `onOrderAccepted`.

    Order buyOrder;
    buyOrder.clOrderId = "1001";
    buyOrder.market = Market::XSHG;
    buyOrder.securityId = "600000";
    buyOrder.side = Side::BUY;
    buyOrder.price = 10.0;
    buyOrder.qty = 1000;
    buyOrder.shareholderId = "SH001";

    riskController.onOrderAccepted(buyOrder);

    Order sellOrder;
    sellOrder.clOrderId = "1002";
    sellOrder.market = Market::XSHG;
    sellOrder.securityId = "600000";
    sellOrder.side = Side::SELL;
    sellOrder.price = 9.0; // Crosses price
    sellOrder.qty = 500;
    sellOrder.shareholderId = "SH001"; // Same shareholder

    // Should detect cross trade
    // EXPECT_EQ(riskController.checkOrder(sellOrder),
    // RiskController::RiskCheckResult::CROSS_TRADE); // Uncomment when
    // implemented
}
