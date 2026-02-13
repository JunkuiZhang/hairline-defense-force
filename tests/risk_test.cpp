#include "risk_controller.h"
#include "types.h"
#include <gtest/gtest.h>

using namespace hdf;

/**
 * @brief 风控引擎测试类
 *
 * 使用 Google Test 的测试夹具（Test
 * Fixture）为每个测试用例提供公共的初始化和清理。
 */
class RiskControllerTest : public testing::Test {
  protected:
    // 每个测试用例都会创建一个新的风控控制器实例
    RiskController riskController;

    /**
     * @brief 辅助函数：创建订单对象
     *
     * @param clOrderId 客户订单ID
     * @param shareholderId 股东号
     * @param securityId 股票代码
     * @param side 买卖方向
     * @param price 价格
     * @param qty 数量
     * @return 创建的订单对象
     */
    Order createOrder(const std::string &clOrderId,
                      const std::string &shareholderId,
                      const std::string &securityId, Side side, double price,
                      uint32_t qty) {
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

/**
 * @brief 测试：空订单簿时不应检测到对敲
 *
 * 验证当风控引擎中没有任何活跃订单时，新订单应该通过风控检查。
 */
TEST_F(RiskControllerTest, EmptyOrderBookNoCrossTrade) {
    // 创建一个买单
    Order buyOrder =
        createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);

    // 验证：空订单簿时应该通过风控检查
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);
}

/**
 * @brief 测试：同股东号反方向订单应检测到对敲
 *
 * 验证当同一股东在同一股票上同时持有买单和卖单时，应检测到对敲风险。
 */
TEST_F(RiskControllerTest, CrossTradeDetectionSameShareholder) {
    // 第一步：创建并接受一个买单
    Order buyOrder =
        createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder);

    // 第二步：创建一个卖单，与买单同股东号、同股票
    Order sellOrder =
        createOrder("1002", "SH001", "600000", Side::SELL, 9.0, 500);
    // 验证：应该检测到对敲
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);
}

/**
 * @brief 测试：不同股东号不应误报对敲
 *
 * 验证当不同股东在同一股票上持有买卖订单时，不应检测到对敲。
 */
TEST_F(RiskControllerTest, NoCrossTradeDifferentShareholder) {
    // 第一步：创建并接受股东SH001的买单
    Order buyOrder =
        createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder);

    // 第二步：创建股东SH002的卖单
    Order sellOrder =
        createOrder("1002", "SH002", "600000", Side::SELL, 9.0, 500);
    // 验证：不同股东号不应检测到对敲
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::PASSED);
}

/**
 * @brief 测试：同方向订单不应误报对敲
 *
 * 验证当同一股东在同一股票上持有多个同方向订单时，不应检测到对敲。
 */
TEST_F(RiskControllerTest, NoCrossTradeSameSide) {
    // 第一步：创建并接受第一个买单
    Order buyOrder1 =
        createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder1),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder1);

    // 第二步：创建第二个买单，与第一个买单同方向
    Order buyOrder2 =
        createOrder("1002", "SH001", "600000", Side::BUY, 9.5, 500);
    // 验证：同方向订单不应检测到对敲
    EXPECT_EQ(riskController.checkOrder(buyOrder2),
              RiskController::RiskCheckResult::PASSED);
}

/**
 * @brief 测试：不同股票不应误报对敲
 *
 * 验证当同一股东在不同股票上持有买卖订单时，不应检测到对敲。
 */
TEST_F(RiskControllerTest, NoCrossTradeDifferentSecurity) {
    // 第一步：创建并接受股票600000的买单
    Order buyOrder =
        createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder);

    // 第二步：创建股票600001的卖单
    Order sellOrder =
        createOrder("1002", "SH001", "600001", Side::SELL, 9.0, 500);
    // 验证：不同股票不应检测到对敲
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::PASSED);
}

/**
 * @brief 测试：订单撤销后对敲状态应更新
 *
 * 验证当订单被撤销后，对敲检测状态应正确更新。
 */
TEST_F(RiskControllerTest, CrossTradeAfterCancel) {
    // 第一步：创建并接受买单
    Order buyOrder =
        createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder);

    // 第二步：创建卖单，应检测到对敲
    Order sellOrder =
        createOrder("1002", "SH001", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);

    // 第三步：撤销买单
    riskController.onOrderCanceled("1001");

    // 验证：撤销后不应再检测到对敲
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::PASSED);
}

/**
 * @brief 测试：完全成交后对敲状态应更新
 *
 * 验证当订单完全成交后，对敲检测状态应正确更新。
 */
TEST_F(RiskControllerTest, CrossTradeAfterFullExecution) {
    // 第一步：创建并接受买单
    Order buyOrder =
        createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder);

    // 第二步：创建卖单，应检测到对敲
    Order sellOrder =
        createOrder("1002", "SH001", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);

    // 第三步：买单完全成交
    riskController.onOrderExecuted("1001", 1000);

    // 验证：完全成交后不应再检测到对敲
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::PASSED);
}

/**
 * @brief 测试：部分成交后对敲状态应正确
 *
 * 验证当订单部分成交后，只要还有剩余数量，仍应检测到对敲。
 */
TEST_F(RiskControllerTest, CrossTradeAfterPartialExecution) {
    // 第一步：创建并接受买单
    Order buyOrder =
        createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(buyOrder);

    // 第二步：创建卖单，应检测到对敲
    Order sellOrder =
        createOrder("1002", "SH001", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);

    // 第三步：买单部分成交（成交500，剩余500）
    riskController.onOrderExecuted("1001", 500);

    // 验证：部分成交后仍应检测到对敲（因为还有剩余数量）
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);
}

/**
 * @brief 测试：多个订单的对敲检测
 *
 * 验证当同一股东在同一股票上持有多个买单时，对敲检测应正确工作。
 */
TEST_F(RiskControllerTest, MultipleOrdersSameShareholder) {
    // 第一步：创建并接受三个买单
    Order buyOrder1 =
        createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 500);
    Order buyOrder2 =
        createOrder("1002", "SH001", "600000", Side::BUY, 10.5, 300);
    Order buyOrder3 =
        createOrder("1003", "SH001", "600000", Side::BUY, 11.0, 200);

    riskController.onOrderAccepted(buyOrder1);
    riskController.onOrderAccepted(buyOrder2);
    riskController.onOrderAccepted(buyOrder3);

    // 第二步：创建卖单，应检测到对敲
    Order sellOrder =
        createOrder("1004", "SH001", "600000", Side::SELL, 9.0, 1000);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);

    // 第三步：部分成交前两个买单
    riskController.onOrderExecuted("1001", 500);
    riskController.onOrderExecuted("1002", 300);

    // 验证：仍有剩余订单时应检测到对敲
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);

    // 第四步：成交第三个买单
    riskController.onOrderExecuted("1003", 200);

    // 验证：所有买单都成交后不应再检测到对敲
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::PASSED);
}

/**
 * @brief 测试：卖单到买单的对敲检测
 *
 * 验证对敲检测在卖单到买单的方向上也能正确工作。
 */
TEST_F(RiskControllerTest, SellToBuyCrossTrade) {
    // 第一步：创建并接受卖单
    Order sellOrder =
        createOrder("1001", "SH001", "600000", Side::SELL, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::PASSED);

    riskController.onOrderAccepted(sellOrder);

    // 第二步：创建买单，应检测到对敲
    Order buyOrder =
        createOrder("1002", "SH001", "600000", Side::BUY, 11.0, 500);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);
}

/**
 * @brief 测试：多个股东号的对敲检测
 *
 * 验证风控引擎能正确处理多个股东号的对敲检测。
 */
TEST_F(RiskControllerTest, MultipleShareholders) {
    // 第一步：创建并接受两个不同股东的买单
    Order buyOrder1 =
        createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    Order buyOrder2 =
        createOrder("1002", "SH002", "600000", Side::BUY, 10.0, 1000);

    riskController.onOrderAccepted(buyOrder1);
    riskController.onOrderAccepted(buyOrder2);

    // 第二步：创建股东SH001的卖单，应检测到对敲
    Order sellOrder1 =
        createOrder("1003", "SH001", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder1),
              RiskController::RiskCheckResult::CROSS_TRADE);

    // 第三步：创建股东SH002的卖单，应检测到对敲
    Order sellOrder2 =
        createOrder("1004", "SH002", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder2),
              RiskController::RiskCheckResult::CROSS_TRADE);

    // 第四步：创建股东SH003的卖单，不应检测到对敲
    Order sellOrder3 =
        createOrder("1005", "SH003", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder3),
              RiskController::RiskCheckResult::PASSED);
}

/**
 * @brief 测试：撤销不存在的订单不应崩溃
 *
 * 验证风控引擎能正确处理撤销不存在订单的情况。
 */
TEST_F(RiskControllerTest, CancelNonExistentOrder) {
    // 撤销一个不存在的订单ID
    riskController.onOrderCanceled("9999");

    // 验证：系统应正常工作，不应崩溃
    EXPECT_EQ(riskController.checkOrder(createOrder("1001", "SH001", "600000",
                                                    Side::BUY, 10.0, 1000)),
              RiskController::RiskCheckResult::PASSED);
}

/**
 * @brief 测试：执行不存在的订单不应崩溃
 *
 * 验证风控引擎能正确处理执行不存在订单的情况。
 */
TEST_F(RiskControllerTest, ExecuteNonExistentOrder) {
    // 执行一个不存在的订单ID
    riskController.onOrderExecuted("9999", 100);

    // 验证：系统应正常工作，不应崩溃
    EXPECT_EQ(riskController.checkOrder(createOrder("1001", "SH001", "600000",
                                                    Side::BUY, 10.0, 1000)),
              RiskController::RiskCheckResult::PASSED);
}
