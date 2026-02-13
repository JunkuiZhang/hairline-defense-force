#include "risk_controller.h"
#include "types.h"
#include <gtest/gtest.h>

using namespace hdf;

class RiskControllerTest : public testing::Test {
  protected:
    RiskController riskController;

    Order createOrder(const std::string &clOrderId, const std::string &shareholderId,
                      const std::string &securityId, Side side, double price, uint32_t qty) {
        Order order;
        order.clOrderId = clOrderId;
        order.market = Market::XSHG;
        order.securityId = securityId;
        order.side = side;
        order.price = price;
        order.qty = qty;
        order.shareholderId = shareholderId;
        return order;
    }
};

TEST_F(RiskControllerTest, EmptyOrderBookNoCrossTrade) {
    Order buyOrder = createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);

    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);
}

TEST_F(RiskControllerTest, CrossTradeDetectionSameShareholder) {
    Order buyOrder = createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder);

    Order sellOrder = createOrder("1002", "SH001", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);
}

TEST_F(RiskControllerTest, NoCrossTradeDifferentShareholder) {
    Order buyOrder = createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder);

    Order sellOrder = createOrder("1002", "SH002", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::PASSED);
}

TEST_F(RiskControllerTest, NoCrossTradeSameSide) {
    Order buyOrder1 = createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder1),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder1);

    Order buyOrder2 = createOrder("1002", "SH001", "600000", Side::BUY, 9.5, 500);
    EXPECT_EQ(riskController.checkOrder(buyOrder2),
              RiskController::RiskCheckResult::PASSED);
}

TEST_F(RiskControllerTest, NoCrossTradeDifferentSecurity) {
    Order buyOrder = createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder);

    Order sellOrder = createOrder("1002", "SH001", "600001", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::PASSED);
}

TEST_F(RiskControllerTest, CrossTradeAfterCancel) {
    Order buyOrder = createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder);

    Order sellOrder = createOrder("1002", "SH001", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);

    riskController.onOrderCanceled("1001");

    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::PASSED);
}

TEST_F(RiskControllerTest, CrossTradeAfterFullExecution) {
    Order buyOrder = createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder);

    Order sellOrder = createOrder("1002", "SH001", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);

    riskController.onOrderExecuted("1001", 1000);

    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::PASSED);
}

TEST_F(RiskControllerTest, CrossTradeAfterPartialExecution) {
    Order buyOrder = createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder);

    Order sellOrder = createOrder("1002", "SH001", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);

    riskController.onOrderExecuted("1001", 500);

    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);
}

TEST_F(RiskControllerTest, MultipleOrdersSameShareholder) {
    Order buyOrder1 = createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 500);
    Order buyOrder2 = createOrder("1002", "SH001", "600000", Side::BUY, 10.5, 300);
    Order buyOrder3 = createOrder("1003", "SH001", "600000", Side::BUY, 11.0, 200);

    riskController.onOrderAccepted(buyOrder1);
    riskController.onOrderAccepted(buyOrder2);
    riskController.onOrderAccepted(buyOrder3);

    Order sellOrder = createOrder("1004", "SH001", "600000", Side::SELL, 9.0, 1000);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);

    riskController.onOrderExecuted("1001", 500);
    riskController.onOrderExecuted("1002", 300);

    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);

    riskController.onOrderExecuted("1003", 200);

    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::PASSED);
}

TEST_F(RiskControllerTest, SellToBuyCrossTrade) {
    Order sellOrder = createOrder("1001", "SH001", "600000", Side::SELL, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(sellOrder);

    Order buyOrder = createOrder("1002", "SH001", "600000", Side::BUY, 11.0, 500);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);
}

TEST_F(RiskControllerTest, MultipleShareholders) {
    Order buyOrder1 = createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    Order buyOrder2 = createOrder("1002", "SH002", "600000", Side::BUY, 10.0, 1000);

    riskController.onOrderAccepted(buyOrder1);
    riskController.onOrderAccepted(buyOrder2);

    Order sellOrder1 = createOrder("1003", "SH001", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder1),
              RiskController::RiskCheckResult::CROSS_TRADE);

    Order sellOrder2 = createOrder("1004", "SH002", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder2),
              RiskController::RiskCheckResult::CROSS_TRADE);

    Order sellOrder3 = createOrder("1005", "SH003", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder3),
              RiskController::RiskCheckResult::PASSED);
}

TEST_F(RiskControllerTest, CancelNonExistentOrder) {
    riskController.onOrderCanceled("9999");
    EXPECT_EQ(riskController.checkOrder(
                  createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000)),
              RiskController::RiskCheckResult::PASSED);
}

TEST_F(RiskControllerTest, ExecuteNonExistentOrder) {
    riskController.onOrderExecuted("9999", 100);
    EXPECT_EQ(riskController.checkOrder(
                  createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000)),
              RiskController::RiskCheckResult::PASSED);
}
