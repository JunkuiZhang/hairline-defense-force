#include "matching_engine.h"
#include "types.h"
#include <gtest/gtest.h>
#include <set>

using namespace hdf;

// ============================================================
// 测试夹具：为每个测试用例提供新的撮合引擎实例和辅助函数
// ============================================================
class MatchingEngineTest : public ::testing::Test {
  protected:
    MatchingEngine engine;

    /**
     * @brief 辅助函数：创建订单对象
     *
     * @param clOrderId 客户订单ID
     * @param securityId 股票代码
     * @param side 买卖方向
     * @param price 价格
     * @param qty 数量
     * @param shareholderId 股东号（默认 "SH001"）
     * @return 创建的订单对象
     */
    Order createOrder(const std::string &clOrderId,
                      const std::string &securityId, Side side, double price,
                      uint32_t qty,
                      const std::string &shareholderId = "SH001") {
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

// ============================================================
// 基础功能测试
// ============================================================

/**
 * @brief 测试：空订单簿时无法撮合
 *
 * 验证当订单簿为空时，提交订单应返回 nullopt（无匹配）。
 */
TEST_F(MatchingEngineTest, EmptyBookNoMatch) {
    Order buyOrder = createOrder("1001", "600030", Side::BUY, 10.0, 1000);
    auto result = engine.match(buyOrder);

    EXPECT_FALSE(result.has_value());
}

/**
 * @brief 测试：等价完全匹配 — 买卖同价同量，成交1笔
 *
 * 验证最简单的撮合场景：一个买单和一个卖单，价格相同，数量相同。
 */
TEST_F(MatchingEngineTest, ExactMatch) {
    // 先挂一个买单到订单簿
    Order buyOrder = createOrder("1001", "600030", Side::BUY, 10.0, 1000);
    engine.addOrder(buyOrder);

    // 提交一个卖单进行撮合
    Order sellOrder =
        createOrder("1002", "600030", Side::SELL, 10.0, 1000, "SH002");
    auto result = engine.match(sellOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions.size(), 1);
    EXPECT_EQ(result->executions[0].clOrderId, "1001");      // 对手方订单ID
    EXPECT_EQ(result->executions[0].execQty, 1000);          // 成交数量
    EXPECT_DOUBLE_EQ(result->executions[0].execPrice, 10.0); // 成交价 = maker价
    EXPECT_EQ(result->remainingQty, 0);                      // 完全成交
}

/**
 * @brief 测试：卖单完全成交，买单部分成交
 *
 * 买方挂1000股，卖方提交500股，卖方应完全成交，
 * 买方订单簿中应剩余500股。
 */
TEST_F(MatchingEngineTest, SimpleMatch) {
    Order buyOrder = createOrder("2001", "600030", Side::BUY, 10.0, 1000);
    engine.addOrder(buyOrder);

    Order sellOrder =
        createOrder("2002", "600030", Side::SELL, 10.0, 500, "SH002");
    auto result = engine.match(sellOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions.size(), 1);
    EXPECT_EQ(result->executions[0].clOrderId, "2001");
    EXPECT_EQ(result->executions[0].execQty, 500);
    EXPECT_EQ(result->remainingQty, 0); // 卖单完全成交
}

/**
 * @brief 测试：买单部分成交，有剩余量
 *
 * 卖方挂500股，买方提交1000股，买方应有500股剩余。
 */
TEST_F(MatchingEngineTest, PartialMatchWithRemainder) {
    Order sellOrder =
        createOrder("3001", "600030", Side::SELL, 10.0, 500, "SH002");
    engine.addOrder(sellOrder);

    Order buyOrder = createOrder("3002", "600030", Side::BUY, 10.0, 1000);
    auto result = engine.match(buyOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions.size(), 1);
    EXPECT_EQ(result->executions[0].execQty, 500);
    EXPECT_EQ(result->remainingQty, 500); // 买单剩余500股
}

/**
 * @brief 测试：价格不匹配时不成交
 *
 * 验证买入价低于卖出价时不应成交。
 */
TEST_F(MatchingEngineTest, PriceMismatchNoMatch) {
    // 挂卖单：卖价11.0
    Order sellOrder =
        createOrder("4001", "600030", Side::SELL, 11.0, 1000, "SH002");
    engine.addOrder(sellOrder);

    // 提交买单：买价10.0（低于卖价11.0）
    Order buyOrder = createOrder("4002", "600030", Side::BUY, 10.0, 1000);
    auto result = engine.match(buyOrder);

    EXPECT_FALSE(result.has_value()); // 不应成交
}

// ============================================================
// 价格优先测试
// ============================================================

/**
 * @brief 测试：价格优先 — 买方优先匹配最低卖价
 *
 * 订单簿中有两个不同卖价的卖单，买方应先匹配更低价格的卖单。
 */
TEST_F(MatchingEngineTest, PricePriorityBuyMatchesLowestAsk) {
    // 先挂高价卖单
    Order sellHigh =
        createOrder("5001", "600030", Side::SELL, 11.0, 500, "SH002");
    engine.addOrder(sellHigh);

    // 再挂低价卖单
    Order sellLow =
        createOrder("5002", "600030", Side::SELL, 10.0, 500, "SH003");
    engine.addOrder(sellLow);

    // 买方以11.0的价格买500股，应该先匹配10.0的卖单
    Order buyOrder = createOrder("5003", "600030", Side::BUY, 11.0, 500);
    auto result = engine.match(buyOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions.size(), 1);
    EXPECT_EQ(result->executions[0].clOrderId, "5002");      // 应匹配低价卖单
    EXPECT_DOUBLE_EQ(result->executions[0].execPrice, 10.0); // 成交价为maker价
    EXPECT_EQ(result->remainingQty, 0);
}

/**
 * @brief 测试：价格优先 — 卖方优先匹配最高买价
 *
 * 订单簿中有两个不同买价的买单，卖方应先匹配更高价格的买单。
 */
TEST_F(MatchingEngineTest, PricePrioritySellMatchesHighestBid) {
    // 先挂低价买单
    Order buyLow = createOrder("6001", "600030", Side::BUY, 9.0, 500);
    engine.addOrder(buyLow);

    // 再挂高价买单
    Order buyHigh =
        createOrder("6002", "600030", Side::BUY, 10.0, 500, "SH002");
    engine.addOrder(buyHigh);

    // 卖方以9.0的价格卖500股，应该先匹配10.0的买单
    Order sellOrder =
        createOrder("6003", "600030", Side::SELL, 9.0, 500, "SH003");
    auto result = engine.match(sellOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions.size(), 1);
    EXPECT_EQ(result->executions[0].clOrderId, "6002");      // 应匹配高价买单
    EXPECT_DOUBLE_EQ(result->executions[0].execPrice, 10.0); // 成交价为maker价
    EXPECT_EQ(result->remainingQty, 0);
}

// ============================================================
// 时间优先测试
// ============================================================

/**
 * @brief 测试：时间优先 — 同价格先挂单先成交
 *
 * 同价格下有多个订单时，先到的订单应该先被匹配。
 */
TEST_F(MatchingEngineTest, TimePrioritySamePrice) {
    // 先挂的订单
    Order sellFirst =
        createOrder("7001", "600030", Side::SELL, 10.0, 500, "SH002");
    engine.addOrder(sellFirst);

    // 后挂的订单（同价格）
    Order sellSecond =
        createOrder("7002", "600030", Side::SELL, 10.0, 500, "SH003");
    engine.addOrder(sellSecond);

    // 买方买500股，应该匹配先挂的7001
    Order buyOrder = createOrder("7003", "600030", Side::BUY, 10.0, 500);
    auto result = engine.match(buyOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions.size(), 1);
    EXPECT_EQ(result->executions[0].clOrderId, "7001"); // 时间优先，匹配先挂的
}

// ============================================================
// 部分成交测试（B5）
// ============================================================

/**
 * @brief 测试：一笔订单匹配多个对手方
 *
 * 大单拆小单：买方提交1000股，匹配两个500股的卖单。
 */
TEST_F(MatchingEngineTest, MultipleMatchesPartialFill) {
    // 挂两个500股的卖单
    Order sell1 = createOrder("8001", "600030", Side::SELL, 10.0, 500, "SH002");
    engine.addOrder(sell1);

    Order sell2 = createOrder("8002", "600030", Side::SELL, 10.0, 500, "SH003");
    engine.addOrder(sell2);

    // 买方买1000股
    Order buyOrder = createOrder("8003", "600030", Side::BUY, 10.0, 1000);
    auto result = engine.match(buyOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions.size(), 2); // 两笔成交
    EXPECT_EQ(result->executions[0].execQty, 500);
    EXPECT_EQ(result->executions[1].execQty, 500);
    EXPECT_EQ(result->remainingQty, 0); // 完全成交
}

/**
 * @brief 测试：跨越多个价格档位的部分成交
 *
 * 买方提交1000股，先匹配低价的500股，再匹配高价的500股。
 */
TEST_F(MatchingEngineTest, MultiPriceLevelMatch) {
    // 低价挂500股
    Order sell1 = createOrder("9001", "600030", Side::SELL, 10.0, 500, "SH002");
    engine.addOrder(sell1);

    // 高价挂500股
    Order sell2 = createOrder("9002", "600030", Side::SELL, 10.5, 500, "SH003");
    engine.addOrder(sell2);

    // 买方以10.5买1000股，应匹配两个价格档位
    Order buyOrder = createOrder("9003", "600030", Side::BUY, 10.5, 1000);
    auto result = engine.match(buyOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions.size(), 2);
    EXPECT_EQ(result->executions[0].clOrderId, "9001"); // 先匹配低价
    EXPECT_DOUBLE_EQ(result->executions[0].execPrice, 10.0);
    EXPECT_EQ(result->executions[1].clOrderId, "9002"); // 再匹配高价
    EXPECT_DOUBLE_EQ(result->executions[1].execPrice, 10.5);
    EXPECT_EQ(result->remainingQty, 0);
}

/**
 * @brief 测试：大单部分成交后有剩余量
 *
 * 订单簿中只有300股，买方提交1000股，应成交300股，剩余700股。
 */
TEST_F(MatchingEngineTest, LargeOrderPartialFillWithRemainder) {
    Order sell1 =
        createOrder("10001", "600030", Side::SELL, 10.0, 300, "SH002");
    engine.addOrder(sell1);

    Order buyOrder = createOrder("10002", "600030", Side::BUY, 10.0, 1000);
    auto result = engine.match(buyOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions.size(), 1);
    EXPECT_EQ(result->executions[0].execQty, 300);
    EXPECT_EQ(result->remainingQty, 700); // 剩余700股
}

// ============================================================
// 成交价测试（B4）
// ============================================================

/**
 * @brief 测试：成交价为被动方（maker）挂单价格
 *
 * 买方以更高价格买入，成交价应该是卖方的挂单价格。
 */
TEST_F(MatchingEngineTest, MakerPriceExecution) {
    // 卖方挂单价格10.0
    Order sellOrder =
        createOrder("11001", "600030", Side::SELL, 10.0, 500, "SH002");
    engine.addOrder(sellOrder);

    // 买方出价10.5（高于卖价）
    Order buyOrder = createOrder("11002", "600030", Side::BUY, 10.5, 500);
    auto result = engine.match(buyOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->executions[0].execPrice, 10.0); // 成交价 = maker价
}

/**
 * @brief 测试：卖方主动成交时，成交价为买方挂单价
 */
TEST_F(MatchingEngineTest, MakerPriceWhenSellerIsTaker) {
    // 买方挂单价格10.5
    Order buyOrder = createOrder("12001", "600030", Side::BUY, 10.5, 500);
    engine.addOrder(buyOrder);

    // 卖方出价10.0（低于买价）
    Order sellOrder =
        createOrder("12002", "600030", Side::SELL, 10.0, 500, "SH002");
    auto result = engine.match(sellOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->executions[0].execPrice,
                     10.5); // 成交价 = maker买价
}

// ============================================================
// 零股处理测试（B6）
// ============================================================

/**
 * @brief 测试：卖出单可以不是100股的整数倍（零股卖出）
 *
 * 卖方挂50股零股，买方以匹配价买入。
 */
TEST_F(MatchingEngineTest, OddLotSellOrder) {
    // 卖方挂50股零股
    Order sellOrder =
        createOrder("13001", "600030", Side::SELL, 10.0, 50, "SH002");
    engine.addOrder(sellOrder);

    // 买方买100股（100的整数倍）
    Order buyOrder = createOrder("13002", "600030", Side::BUY, 10.0, 100);
    auto result = engine.match(buyOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions.size(), 1);
    EXPECT_EQ(result->executions[0].execQty, 50); // 成交50股零股
    EXPECT_EQ(result->remainingQty, 50);          // 买方剩余50股
}

/**
 * @brief 测试：零股与正常股票混合撮合
 *
 * 订单簿有50股零股和500股整手，买方提交200股。
 */
TEST_F(MatchingEngineTest, MixedOddAndRoundLot) {
    // 先挂50股零股
    Order sell1 = createOrder("14001", "600030", Side::SELL, 10.0, 50, "SH002");
    engine.addOrder(sell1);

    // 再挂500股整手
    Order sell2 =
        createOrder("14002", "600030", Side::SELL, 10.0, 500, "SH003");
    engine.addOrder(sell2);

    // 买方买200股
    Order buyOrder = createOrder("14003", "600030", Side::BUY, 10.0, 200);
    auto result = engine.match(buyOrder);

    ASSERT_TRUE(result.has_value());
    // 先匹配50股零股，然后匹配剩余150→100股（取100的整数倍）
    ASSERT_EQ(result->executions.size(), 2u); // 应该有两笔成交：50 + 100
    EXPECT_EQ(result->executions[0].execQty, 50u);
    EXPECT_EQ(result->executions[1].execQty, 100u);

    uint32_t totalExecQty = 0;
    for (const auto &exec : result->executions) {
        totalExecQty += exec.execQty;
    }
    // 50 + 100 = 150
    EXPECT_EQ(totalExecQty, 150u);
    // 剩余未成交数量应为 50 股
    EXPECT_EQ(result->remainingQty, 50u);
}

// ============================================================
// 撤单测试（B7）
// ============================================================

/**
 * @brief 测试：撤单正确移除订单并返回信息
 *
 * 挂单后撤单，验证返回的 CancelResponse 信息正确。
 */
TEST_F(MatchingEngineTest, CancelOrderSuccess) {
    Order buyOrder = createOrder("15001", "600030", Side::BUY, 10.0, 1000);
    engine.addOrder(buyOrder);

    CancelResponse resp = engine.cancelOrder("15001");

    EXPECT_EQ(resp.type, CancelResponse::Type::CONFIRM);
    EXPECT_EQ(resp.origClOrderId, "15001");
    EXPECT_EQ(resp.clOrderId, "15001");
    EXPECT_EQ(resp.securityId, "600030");
    EXPECT_EQ(resp.side, Side::BUY);
    EXPECT_DOUBLE_EQ(resp.price, 10.0);
    EXPECT_EQ(resp.qty, 1000);
    EXPECT_EQ(resp.cumQty, 0);         // 未成交
    EXPECT_EQ(resp.canceledQty, 1000); // 全部撤销
}

/**
 * @brief 测试：撤销不存在的订单返回拒绝
 */
TEST_F(MatchingEngineTest, CancelNonexistentOrder) {
    CancelResponse resp = engine.cancelOrder("NONEXISTENT");

    EXPECT_EQ(resp.type, CancelResponse::Type::REJECT);
    EXPECT_EQ(resp.origClOrderId, "NONEXISTENT");
}

/**
 * @brief 测试：撤单后订单不再参与撮合
 *
 * 挂单后撤单，再提交可匹配的订单，应无法成交。
 */
TEST_F(MatchingEngineTest, CancelOrderThenNoMatch) {
    Order sellOrder =
        createOrder("16001", "600030", Side::SELL, 10.0, 500, "SH002");
    engine.addOrder(sellOrder);

    // 撤单
    engine.cancelOrder("16001");

    // 再提交可匹配的买单
    Order buyOrder = createOrder("16002", "600030", Side::BUY, 10.0, 500);
    auto result = engine.match(buyOrder);

    EXPECT_FALSE(result.has_value()); // 撤单后不应匹配
}

/**
 * @brief 测试：部分成交后撤单，返回正确的已成交累计量
 *
 * 挂1000股，成交500股后撤单，cumQty 应为500，canceledQty 应为500。
 */
TEST_F(MatchingEngineTest, CancelAfterPartialFill) {
    // 挂1000股买单
    Order buyOrder = createOrder("17001", "600030", Side::BUY, 10.0, 1000);
    engine.addOrder(buyOrder);

    // 卖500股，部分成交
    Order sellOrder =
        createOrder("17002", "600030", Side::SELL, 10.0, 500, "SH002");
    auto result = engine.match(sellOrder);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions[0].execQty, 500);

    // 撤剩余的单
    CancelResponse resp = engine.cancelOrder("17001");
    EXPECT_EQ(resp.type, CancelResponse::Type::CONFIRM);
    EXPECT_EQ(resp.cumQty, 500);      // 已成交500
    EXPECT_EQ(resp.canceledQty, 500); // 撤销剩余500
}

// ============================================================
// reduceOrderQty 测试（B8）
// ============================================================

/**
 * @brief 测试：减少订单数量后正确影响撮合
 *
 * 挂1000股，减少400股后，应只能匹配600股。
 */
TEST_F(MatchingEngineTest, ReduceOrderQtyBasic) {
    Order buyOrder = createOrder("18001", "600030", Side::BUY, 10.0, 1000);
    engine.addOrder(buyOrder);

    // 减少400股
    engine.reduceOrderQty("18001", 400);

    // 尝试匹配1000股的卖单，应只能成交600股
    Order sellOrder =
        createOrder("18002", "600030", Side::SELL, 10.0, 1000, "SH002");
    auto result = engine.match(sellOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions[0].execQty, 600);
    EXPECT_EQ(result->remainingQty, 400); // 卖单剩余400
}

/**
 * @brief 测试：减少数量归零后自动移除
 *
 * 挂500股，减少500股，订单应自动移除，无法再匹配。
 */
TEST_F(MatchingEngineTest, ReduceOrderQtyToZero) {
    Order sellOrder =
        createOrder("19001", "600030", Side::SELL, 10.0, 500, "SH002");
    engine.addOrder(sellOrder);

    // 减少到0
    engine.reduceOrderQty("19001", 500);

    // 应无法匹配
    Order buyOrder = createOrder("19002", "600030", Side::BUY, 10.0, 500);
    auto result = engine.match(buyOrder);

    EXPECT_FALSE(result.has_value());
}

/**
 * @brief 测试：对不存在的订单执行减少操作不崩溃
 */
TEST_F(MatchingEngineTest, ReduceNonexistentOrder) {
    // 不应抛出异常或崩溃
    EXPECT_NO_THROW(engine.reduceOrderQty("NONEXISTENT", 100));
}

// ============================================================
// execId 生成测试（B9）
// ============================================================

/**
 * @brief 测试：每笔成交的 execId 唯一
 *
 * 多次撮合产生的 execId 应各不相同。
 */
TEST_F(MatchingEngineTest, UniqueExecIds) {
    // 挂两个卖单
    Order sell1 =
        createOrder("20001", "600030", Side::SELL, 10.0, 500, "SH002");
    engine.addOrder(sell1);

    Order sell2 =
        createOrder("20002", "600030", Side::SELL, 10.0, 500, "SH003");
    engine.addOrder(sell2);

    // 买方匹配两个
    Order buyOrder = createOrder("20003", "600030", Side::BUY, 10.0, 1000);
    auto result = engine.match(buyOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions.size(), 2);

    // 验证 execId 不为空且唯一
    std::set<std::string> execIds;
    for (const auto &exec : result->executions) {
        EXPECT_FALSE(exec.execId.empty());
        execIds.insert(exec.execId);
    }
    EXPECT_EQ(execIds.size(), 2); // 两个不同的 execId
}

/**
 * @brief 测试：execId 跨多次撮合仍然唯一
 */
TEST_F(MatchingEngineTest, UniqueExecIdsAcrossMatches) {
    std::set<std::string> allExecIds;

    // 第一次撮合
    Order sell1 =
        createOrder("21001", "600030", Side::SELL, 10.0, 500, "SH002");
    engine.addOrder(sell1);
    Order buy1 = createOrder("21002", "600030", Side::BUY, 10.0, 500);
    auto result1 = engine.match(buy1);
    ASSERT_TRUE(result1.has_value());
    allExecIds.insert(result1->executions[0].execId);

    // 第二次撮合
    Order sell2 =
        createOrder("21003", "600030", Side::SELL, 10.0, 500, "SH003");
    engine.addOrder(sell2);
    Order buy2 = createOrder("21004", "600030", Side::BUY, 10.0, 500, "SH004");
    auto result2 = engine.match(buy2);
    ASSERT_TRUE(result2.has_value());
    allExecIds.insert(result2->executions[0].execId);

    EXPECT_EQ(allExecIds.size(), 2); // 两个不同的 execId
}

// ============================================================
// 不同股票隔离测试
// ============================================================

/**
 * @brief 测试：不同股票的订单不会互相撮合
 */
TEST_F(MatchingEngineTest, DifferentSecurityNoMatch) {
    // 挂600030的卖单
    Order sellOrder =
        createOrder("22001", "600030", Side::SELL, 10.0, 500, "SH002");
    engine.addOrder(sellOrder);

    // 提交600031的买单（不同股票）
    Order buyOrder = createOrder("22002", "600031", Side::BUY, 10.0, 500);
    auto result = engine.match(buyOrder);

    EXPECT_FALSE(result.has_value()); // 不同股票不应匹配
}

// ============================================================
// 复杂场景测试
// ============================================================

/**
 * @brief 测试：多档价格 + 多笔订单的综合撮合
 *
 * 订单簿中有多个价格档位的卖单，买方大单依次匹配。
 */
TEST_F(MatchingEngineTest, ComplexMultiLevelMatch) {
    // 挂3个不同价格的卖单
    Order sell1 =
        createOrder("23001", "600030", Side::SELL, 10.0, 300, "SH002");
    engine.addOrder(sell1);

    Order sell2 =
        createOrder("23002", "600030", Side::SELL, 10.5, 400, "SH003");
    engine.addOrder(sell2);

    Order sell3 =
        createOrder("23003", "600030", Side::SELL, 11.0, 500, "SH004");
    engine.addOrder(sell3);

    // 买方以11.0买1000股
    Order buyOrder = createOrder("23004", "600030", Side::BUY, 11.0, 1000);
    auto result = engine.match(buyOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions.size(), 3); // 匹配3笔

    // 验证价格优先：先10.0，再10.5，再11.0
    EXPECT_DOUBLE_EQ(result->executions[0].execPrice, 10.0);
    EXPECT_EQ(result->executions[0].execQty, 300);

    EXPECT_DOUBLE_EQ(result->executions[1].execPrice, 10.5);
    EXPECT_EQ(result->executions[1].execQty, 400);

    EXPECT_DOUBLE_EQ(result->executions[2].execPrice, 11.0);
    EXPECT_EQ(result->executions[2].execQty, 300); // 只需300股即可满足

    EXPECT_EQ(result->remainingQty, 0);
}

/**
 * @brief 测试：连续撮合后订单簿状态正确
 *
 * 多次撮合操作后，验证订单簿状态的一致性。
 */
TEST_F(MatchingEngineTest, ConsecutiveMatchesBookState) {
    // 挂1000股买单
    Order buy1 = createOrder("24001", "600030", Side::BUY, 10.0, 1000);
    engine.addOrder(buy1);

    // 第一次卖单：成交400股
    Order sell1 =
        createOrder("24002", "600030", Side::SELL, 10.0, 400, "SH002");
    auto result1 = engine.match(sell1);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1->executions[0].execQty, 400);

    // 第二次卖单：应还能匹配600股
    Order sell2 =
        createOrder("24003", "600030", Side::SELL, 10.0, 800, "SH003");
    auto result2 = engine.match(sell2);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2->executions[0].execQty, 600); // 买单剩余600
    EXPECT_EQ(result2->remainingQty, 200);          // 卖单剩余200

    // 第三次卖单：买单已全部成交，不应匹配
    Order sell3 =
        createOrder("24004", "600030", Side::SELL, 10.0, 100, "SH004");
    auto result3 = engine.match(sell3);
    EXPECT_FALSE(result3.has_value());
}

/**
 * @brief 测试：addOrder 后可以正常被匹配
 *
 * 多次 addOrder 后批量撮合。
 */
TEST_F(MatchingEngineTest, AddMultipleOrdersThenMatch) {
    // 挂多个买单
    engine.addOrder(createOrder("25001", "600030", Side::BUY, 10.0, 200));
    engine.addOrder(
        createOrder("25002", "600030", Side::BUY, 10.2, 300, "SH002"));
    engine.addOrder(
        createOrder("25003", "600030", Side::BUY, 9.8, 400, "SH003"));

    // 卖方以9.8价格卖600股
    Order sellOrder =
        createOrder("25004", "600030", Side::SELL, 9.8, 600, "SH004");
    auto result = engine.match(sellOrder);

    ASSERT_TRUE(result.has_value());
    // 应先匹配价格最高的买单10.2（300股），再匹配10.0（200股），再匹配9.8（100股）
    EXPECT_EQ(result->executions.size(), 3);
    EXPECT_EQ(result->executions[0].clOrderId, "25002"); // 10.2最先
    EXPECT_EQ(result->executions[0].execQty, 300);
    EXPECT_EQ(result->executions[1].clOrderId, "25001"); // 10.0其次
    EXPECT_EQ(result->executions[1].execQty, 200);
    EXPECT_EQ(result->executions[2].clOrderId, "25003"); // 9.8最后
    EXPECT_EQ(result->executions[2].execQty, 100);
    EXPECT_EQ(result->remainingQty, 0);
}

/**
 * @brief 测试：撤销卖方订单
 */
TEST_F(MatchingEngineTest, CancelSellOrder) {
    Order sellOrder =
        createOrder("26001", "600030", Side::SELL, 11.0, 800, "SH002");
    engine.addOrder(sellOrder);

    CancelResponse resp = engine.cancelOrder("26001");

    EXPECT_EQ(resp.type, CancelResponse::Type::CONFIRM);
    EXPECT_EQ(resp.origClOrderId, "26001");
    EXPECT_EQ(resp.side, Side::SELL);
    EXPECT_EQ(resp.canceledQty, 800);
    EXPECT_EQ(resp.cumQty, 0);
}

/**
 * @brief 测试：reduceOrderQty 对卖方订单生效
 */
TEST_F(MatchingEngineTest, ReduceSellOrderQty) {
    Order sellOrder =
        createOrder("27001", "600030", Side::SELL, 10.0, 1000, "SH002");
    engine.addOrder(sellOrder);

    engine.reduceOrderQty("27001", 300);

    Order buyOrder = createOrder("27002", "600030", Side::BUY, 10.0, 1000);
    auto result = engine.match(buyOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->executions[0].execQty, 700); // 1000 - 300 = 700
    EXPECT_EQ(result->remainingQty, 300);
}

/**
 * @brief 测试：成交完全消耗对手方后正确移除
 *
 * 验证完全成交的对手方订单不会残留在订单簿中。
 */
TEST_F(MatchingEngineTest, FullyConsumedOrderRemoved) {
    Order sellOrder =
        createOrder("28001", "600030", Side::SELL, 10.0, 500, "SH002");
    engine.addOrder(sellOrder);

    // 完全成交
    Order buyOrder = createOrder("28002", "600030", Side::BUY, 10.0, 500);
    auto result = engine.match(buyOrder);
    ASSERT_TRUE(result.has_value());

    // 撤单应返回拒绝（已经被完全成交移除了）
    CancelResponse resp = engine.cancelOrder("28001");
    EXPECT_EQ(resp.type, CancelResponse::Type::REJECT);
}

/**
 * @brief 测试：OrderResponse 结构体中各字段正确填充
 */
TEST_F(MatchingEngineTest, ExecutionResponseFieldsCorrect) {
    Order sellOrder =
        createOrder("29001", "600030", Side::SELL, 10.5, 300, "SH002");
    engine.addOrder(sellOrder);

    Order buyOrder = createOrder("29002", "600030", Side::BUY, 10.5, 300);
    auto result = engine.match(buyOrder);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->executions.size(), 1);

    const auto &exec = result->executions[0];
    EXPECT_EQ(exec.clOrderId, "29001");
    EXPECT_EQ(exec.market, Market::XSHG);
    EXPECT_EQ(exec.securityId, "600030");
    EXPECT_EQ(exec.side, Side::SELL);
    EXPECT_EQ(exec.qty, 300);           // 原始委托量
    EXPECT_DOUBLE_EQ(exec.price, 10.5); // 原始委托价
    EXPECT_EQ(exec.shareholderId, "SH002");
    EXPECT_EQ(exec.execQty, 300);           // 成交数量
    EXPECT_DOUBLE_EQ(exec.execPrice, 10.5); // 成交价
    EXPECT_FALSE(exec.execId.empty());      // execId 非空
    EXPECT_EQ(exec.type, OrderResponse::Type::EXECUTION);
}

// ============================================================
// 卖方零股吃单测试（Review 反馈补充）
// ============================================================

/**
 * @brief 测试：卖出零股作为吃单方与买方挂单撮合
 *
 * 买方订单簿有200股整手买单，卖方提交150股卖单作为吃单。
 * 预期：为防止买方被留下零股余量，撮合数量调整为100股。
 */
TEST_F(MatchingEngineTest, OddLotSellTakerAgainstBuyOrder) {
    // 先挂200股买单作为挂单方
    Order buyOrder = createOrder("30001", "600030", Side::BUY, 10.0, 200);
    engine.addOrder(buyOrder);

    // 卖方提交150股作为吃单方
    Order sellOrder =
        createOrder("30002", "600030", Side::SELL, 10.0, 150, "SH002");
    auto result = engine.match(sellOrder);

    ASSERT_TRUE(result.has_value());
    EXPECT_GE(result->executions.size(), 1u);

    uint32_t totalExecQty = 0;
    for (const auto &exec : result->executions) {
        totalExecQty += exec.execQty;
    }

    // 卖出150股，仅成交100股（防止买方被留下50股零股）
    EXPECT_EQ(totalExecQty, 100u);
}

// ============================================================
// 重复 clOrderId 测试（Review 反馈补充）
// ============================================================

/**
 * @brief 测试：重复 clOrderId 的订单应被忽略
 *
 * 同一个 clOrderId 添加两次，第二次应被忽略，
 * 订单簿中只保留第一笔订单。
 */
TEST_F(MatchingEngineTest, DuplicateClOrderIdIgnored) {
    Order order1 = createOrder("31001", "600030", Side::BUY, 10.0, 100);
    Order order2 = createOrder("31001", "600030", Side::BUY, 11.0, 200);

    engine.addOrder(order1);
    engine.addOrder(order2); // 重复 ID，应被忽略

    // 撤单应返回第一笔订单的信息
    CancelResponse resp = engine.cancelOrder("31001");
    EXPECT_EQ(resp.type, CancelResponse::Type::CONFIRM);
    EXPECT_EQ(resp.qty, 100);           // 应为第一笔订单的数量
    EXPECT_DOUBLE_EQ(resp.price, 10.0); // 应为第一笔订单的价格
}
