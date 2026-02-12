#include "matching_engine.h"
#include "types.h"
#include <gtest/gtest.h>

using namespace hdf;

class MatchingEngineTest : public ::testing::Test {
  protected:
    MatchingEngine engine;
};

TEST_F(MatchingEngineTest, SimpleMatch) {
    Order buyOrder;
    buyOrder.clOrderId = "2001";
    buyOrder.market = Market::XSHG;
    buyOrder.securityId = "600030";
    buyOrder.side = Side::BUY;
    buyOrder.price = 10.0;
    buyOrder.qty = 1000;
    buyOrder.shareholderId = "SH001";

    engine.addOrder(buyOrder);

    Order sellOrder;
    sellOrder.clOrderId = "2002";
    sellOrder.market = Market::XSHG;
    sellOrder.securityId = "600030";
    sellOrder.side = Side::SELL;
    sellOrder.price = 10.0;
    sellOrder.qty = 500;
    sellOrder.shareholderId = "SH002";

    auto result = engine.match(sellOrder);

    // 应该匹配成功，成交500股，剩余500股未成交
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->executions.size(), 1);
    EXPECT_EQ(result->executions[0].clOrderId, "2001");
    EXPECT_EQ(result->executions[0].execQty, 500);
    EXPECT_EQ(result->remainingQty, 0); // 卖单完全成交，无剩余
}
