#include "constants.h"
#include "trade_system.h"
#include <gtest/gtest.h>
#include <vector>

using namespace hdf;
using json = nlohmann::json;

// 辅助函数：构造订单 JSON
static json makeOrder(const std::string &clOrderId, const std::string &market,
                      const std::string &securityId, const std::string &side,
                      double price, uint32_t qty,
                      const std::string &shareholderId) {
    return {{"clOrderId", clOrderId},
            {"market", market},
            {"securityId", securityId},
            {"side", side},
            {"price", price},
            {"qty", qty},
            {"shareholderId", shareholderId}};
}

// 辅助函数：构造撤单 JSON
static json makeCancel(const std::string &clOrderId,
                       const std::string &origClOrderId,
                       const std::string &market, const std::string &securityId,
                       const std::string &shareholderId,
                       const std::string &side) {
    return {{"clOrderId", clOrderId},
            {"origClOrderId", origClOrderId},
            {"market", market},
            {"securityId", securityId},
            {"shareholderId", shareholderId},
            {"side", side}};
}

// 纯撮合系统测试基类：不设置 sendToExchange
class ExchangeTest : public testing::Test {
  protected:
    TradeSystem system;
    std::vector<json> clientResponses;

    void SetUp() override {
        system.setSendToClient(
            [this](const json &resp) { clientResponses.push_back(resp); });
        // 不设置 sendToExchange → 纯撮合模式
    }
};

// ==================== 订单确认 ====================

TEST_F(ExchangeTest, SingleOrderNoMatch_Confirm) {
    // 单个买单无对手方，应入簿并返回确认回报
    system.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    ASSERT_EQ(clientResponses.size(), 1);
    auto &resp = clientResponses[0];
    EXPECT_EQ(resp["clOrderId"], "1001");
    EXPECT_EQ(resp["market"], "XSHG");
    EXPECT_EQ(resp["securityId"], "600030");
    EXPECT_EQ(resp["side"], "B");
    EXPECT_EQ(resp["qty"], 100);
    EXPECT_DOUBLE_EQ(resp["price"], 10.0);
    EXPECT_EQ(resp["shareholderId"], "SH001");
    // 确认回报不含 execId
    EXPECT_FALSE(resp.contains("execId"));
}

TEST_F(ExchangeTest, SellOrderNoMatch_Confirm) {
    // 单个卖单无对手方
    system.handleOrder(
        makeOrder("2001", "XSHE", "000001", "S", 20.0, 50, "SZ001"));

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["clOrderId"], "2001");
    EXPECT_EQ(clientResponses[0]["side"], "S");
    EXPECT_EQ(clientResponses[0]["qty"], 50);
}

// ==================== 完全匹配 ====================

TEST_F(ExchangeTest, ExactMatch_TwoExecutionReports) {
    // 先挂买单，再来等价等量卖单 → 完全匹配
    system.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("2001", "XSHG", "600030", "S", 10.0, 100, "SH002"));

    // 应产生2个成交回报：被动方 + 主动方
    ASSERT_EQ(clientResponses.size(), 2);

    // 被动方（买方）成交回报
    auto &passive = clientResponses[0];
    EXPECT_EQ(passive["clOrderId"], "1001");
    EXPECT_EQ(passive["side"], "B");
    EXPECT_EQ(passive["execQty"], 100);
    EXPECT_DOUBLE_EQ(passive["execPrice"].get<double>(), 10.0);
    EXPECT_TRUE(passive.contains("execId"));

    // 主动方（卖方）成交回报
    auto &active = clientResponses[1];
    EXPECT_EQ(active["clOrderId"], "2001");
    EXPECT_EQ(active["side"], "S");
    EXPECT_EQ(active["execQty"], 100);
    EXPECT_DOUBLE_EQ(active["execPrice"].get<double>(), 10.0);

    // 两个回报应有相同的 execId
    EXPECT_EQ(passive["execId"], active["execId"]);
}

// ==================== 部分成交 ====================

TEST_F(ExchangeTest, PartialMatch_RemainingEntersBook) {
    // 买1000股，卖300股 → 成交300，剩余700入簿
    system.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 1000, "SH001"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("2001", "XSHG", "600030", "S", 10.0, 300, "SH002"));

    // 2个成交回报（被动方+主动方），无剩余确认（卖方全部成交）
    ASSERT_EQ(clientResponses.size(), 2);
    EXPECT_EQ(clientResponses[0]["execQty"], 300);
    EXPECT_EQ(clientResponses[1]["execQty"], 300);
}

TEST_F(ExchangeTest, PartialMatch_ActiveHasRemaining) {
    // 卖300股挂簿，买1000股来撮合 → 成交300，买方剩余700入簿+确认回报
    system.handleOrder(
        makeOrder("2001", "XSHG", "600030", "S", 10.0, 300, "SH002"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 1000, "SH001"));

    // 2个成交回报 + 1个剩余入簿确认 = 3
    ASSERT_EQ(clientResponses.size(), 3);

    // 成交回报
    EXPECT_EQ(clientResponses[0]["execQty"], 300); // 被动方
    EXPECT_EQ(clientResponses[1]["execQty"], 300); // 主动方

    // 剩余入簿确认
    auto &confirm = clientResponses[2];
    EXPECT_EQ(confirm["clOrderId"], "1001");
    EXPECT_EQ(confirm["qty"], 700);
    EXPECT_FALSE(confirm.contains("execId"));
}

// ==================== 多对手方匹配 ====================

TEST_F(ExchangeTest, MultipleCounterparties) {
    // 3个卖单挂簿，1个大买单撮合所有
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 200, "SH002"));
    system.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 10.0, 300, "SH003"));
    system.handleOrder(
        makeOrder("S3", "XSHG", "600030", "S", 10.5, 500, "SH004"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.5, 1000, "SH001"));

    // S1成交200, S2成交300, S3成交500 → 各2个回报 = 6个
    ASSERT_EQ(clientResponses.size(), 6);

    // 第一笔成交（S1, 价格优先 10.0）
    EXPECT_EQ(clientResponses[0]["clOrderId"], "S1");
    EXPECT_EQ(clientResponses[0]["execQty"], 200);
    EXPECT_DOUBLE_EQ(clientResponses[0]["execPrice"].get<double>(), 10.0);
    EXPECT_EQ(clientResponses[1]["clOrderId"], "B1");
    EXPECT_EQ(clientResponses[1]["execQty"], 200);

    // 第二笔成交（S2, 同价时间优先）
    EXPECT_EQ(clientResponses[2]["clOrderId"], "S2");
    EXPECT_EQ(clientResponses[2]["execQty"], 300);
    EXPECT_DOUBLE_EQ(clientResponses[2]["execPrice"].get<double>(), 10.0);

    // 第三笔成交（S3, 价格 10.5）
    EXPECT_EQ(clientResponses[4]["clOrderId"], "S3");
    EXPECT_EQ(clientResponses[4]["execQty"], 500);
    EXPECT_DOUBLE_EQ(clientResponses[4]["execPrice"].get<double>(), 10.5);
}

// ==================== 价格优先 ====================

TEST_F(ExchangeTest, PricePriority_BestPriceFirst) {
    // 挂两个卖单：贵的先来，便宜的后来
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 11.0, 100, "SH002"));
    system.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 10.0, 100, "SH003"));
    clientResponses.clear();

    // 买单价格11.0，应优先匹配更便宜的S2
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 11.0, 100, "SH001"));

    ASSERT_EQ(clientResponses.size(), 2);
    EXPECT_EQ(clientResponses[0]["clOrderId"], "S2"); // 10.0先匹配
    EXPECT_DOUBLE_EQ(clientResponses[0]["execPrice"].get<double>(), 10.0);
}

// ==================== 时间优先 ====================

TEST_F(ExchangeTest, TimePriority_EarlierFirst) {
    // 同价两个卖单，S1先来
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    system.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 10.0, 100, "SH003"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    ASSERT_EQ(clientResponses.size(), 2);
    EXPECT_EQ(clientResponses[0]["clOrderId"], "S1"); // S1先挂单先成交
}

// ==================== 成交价（maker price） ====================

TEST_F(ExchangeTest, ExecutionPrice_MakerPrice) {
    // 卖方挂9.5，买方出10.0 → 成交价应为卖方挂单价9.5
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 9.5, 100, "SH002"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    ASSERT_EQ(clientResponses.size(), 2);
    EXPECT_DOUBLE_EQ(clientResponses[0]["execPrice"].get<double>(), 9.5);
    EXPECT_DOUBLE_EQ(clientResponses[1]["execPrice"].get<double>(), 9.5);
}

// ==================== 价格不满足不成交 ====================

TEST_F(ExchangeTest, NoMatch_PriceNotSatisfied) {
    // 卖方要11.0，买方只出10.0 → 不成交，各自入簿
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 11.0, 100, "SH002"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    // 只有确认回报，无成交
    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["clOrderId"], "B1");
    EXPECT_FALSE(clientResponses[0].contains("execId"));
}

// ==================== 不同股票不匹配 ====================

TEST_F(ExchangeTest, DifferentSecurity_NoMatch) {
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("B1", "XSHG", "601318", "B", 10.0, 100, "SH001"));

    // 不同股票，不成交
    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["clOrderId"], "B1");
    EXPECT_FALSE(clientResponses[0].contains("execId"));
}

// ==================== 对敲检测 ====================

TEST_F(ExchangeTest, CrossTrade_Rejected) {
    // 同一股东号先买后卖同一股票 → 对敲拒绝
    system.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("2001", "XSHG", "600030", "S", 10.0, 100, "SH001"));

    ASSERT_EQ(clientResponses.size(), 1);
    auto &resp = clientResponses[0];
    EXPECT_EQ(resp["clOrderId"], "2001");
    EXPECT_EQ(resp["rejectCode"], ORDER_CROSS_TRADE_REJECT_CODE);
    EXPECT_TRUE(resp.contains("rejectText"));
}

TEST_F(ExchangeTest, DifferentShareholderId_NoCrossTrade) {
    // 不同股东号，不算对敲
    system.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("2001", "XSHG", "600030", "S", 10.0, 100, "SH002"));

    // 不被拒绝，应正常成交
    ASSERT_GE(clientResponses.size(), 1);
    EXPECT_FALSE(clientResponses[0].contains("rejectCode"));
}

TEST_F(ExchangeTest, SameDirection_NoCrossTrade) {
    // 同股东号同方向，不算对敲
    system.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("1002", "XSHG", "600030", "B", 10.5, 200, "SH001"));

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_FALSE(clientResponses[0].contains("rejectCode"));
}

// ==================== JSON 解析错误 ====================

TEST_F(ExchangeTest, InvalidOrderJson_Rejected) {
    json badOrder = {{"clOrderId", "BAD1"}, {"market", "INVALID"}};
    system.handleOrder(badOrder);

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["rejectCode"],
              ORDER_INVALID_FORMAT_REJECT_CODE);
}

TEST_F(ExchangeTest, InvalidCancelJson_Rejected) {
    json badCancel = {{"clOrderId", "C1"}};
    system.handleCancel(badCancel);

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["rejectCode"],
              ORDER_INVALID_FORMAT_REJECT_CODE);
}

// ==================== 撤单 ====================

TEST_F(ExchangeTest, CancelOrder_Success) {
    // 挂单后撤单
    system.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    system.handleCancel(
        makeCancel("C001", "1001", "XSHG", "600030", "SH001", "B"));

    ASSERT_EQ(clientResponses.size(), 1);
    auto &resp = clientResponses[0];
    EXPECT_EQ(resp["origClOrderId"], "1001");
    EXPECT_EQ(resp["canceledQty"], 100);
    EXPECT_EQ(resp["cumQty"], 0);
}

TEST_F(ExchangeTest, CancelOrder_ThenNoMatch) {
    // 挂卖单→撤单→买单来了不应匹配已撤的卖单
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    system.handleCancel(
        makeCancel("C001", "S1", "XSHG", "600030", "SH002", "S"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    // 应仅有确认回报（入簿），无成交
    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["clOrderId"], "B1");
    EXPECT_FALSE(clientResponses[0].contains("execId"));
}

// ==================== 对敲 - 成交后状态更新 ====================

TEST_F(ExchangeTest, CrossTrade_AfterFullExecution_NoCrossTrade) {
    // SH001先挂买单，SH002卖单完全成交后，SH001再挂卖单不应对敲
    system.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    system.handleOrder(
        makeOrder("2001", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    clientResponses.clear();

    // 买单已完全成交，此时SH001挂卖单不应对敲
    system.handleOrder(
        makeOrder("3001", "XSHG", "600030", "S", 10.0, 100, "SH001"));

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_FALSE(clientResponses[0].contains("rejectCode"));
}

TEST_F(ExchangeTest, CrossTrade_AfterCancel_NoCrossTrade) {
    // SH001先挂买单→撤单→再挂卖单不应对敲
    system.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    system.handleCancel(
        makeCancel("C001", "1001", "XSHG", "600030", "SH001", "B"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("2001", "XSHG", "600030", "S", 10.0, 100, "SH001"));

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_FALSE(clientResponses[0].contains("rejectCode"));
}

// ==================== 零股 ====================

TEST_F(ExchangeTest, OddLot_SellCanBeOddLot) {
    // 买方100股挂簿，卖方50股来 → 应能成交（卖方可零股）
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 50, "SH002"));

    // 应成交50股
    ASSERT_GE(clientResponses.size(), 2);
    EXPECT_EQ(clientResponses[0]["execQty"], 50);
}

// ==================== 连续交易 ====================

TEST_F(ExchangeTest, SequentialTrades) {
    // 第一笔交易
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 200, "SH001"));
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 200, "SH002"));

    // 第二笔交易（不同股票）
    system.handleOrder(
        makeOrder("B2", "XSHE", "000001", "B", 20.0, 300, "SH003"));
    system.handleOrder(
        makeOrder("S2", "XSHE", "000001", "S", 20.0, 300, "SH004"));

    // 4个确认/成交回报对应第一笔 + 4个对应第二笔 = 多个
    // 简单验证最后一笔成交
    bool foundS2Exec = false;
    for (const auto &resp : clientResponses) {
        if (resp.value("clOrderId", "") == "S2" && resp.contains("execId")) {
            foundS2Exec = true;
            EXPECT_EQ(resp["execQty"], 300);
        }
    }
    EXPECT_TRUE(foundS2Exec);
}
