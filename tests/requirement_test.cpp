/**
 * @file requirement_test.cpp
 * @brief 根据项目书要求一一对应的测试用例
 *
 * 项目书章节:
 *   2.1.1 交易转发
 *   2.1.2 对敲风控
 *   2.1.3 模拟撮合
 *   2.2.1 行情接入
 *   2.2.2 撤单支持
 */

#include "constants.h"
#include "trade_system.h"
#include <gtest/gtest.h>
#include <vector>

using namespace hdf;
using json = nlohmann::json;

// ─── 辅助函数 ──────────────────────────────────────────────────────

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

// ─── 测试基类：交易所前置模式 (gateway ↔ exchange) ──────────────────

class RequirementGatewayTest : public testing::Test {
  protected:
    TradeSystem gateway{1};
    TradeSystem exchange{1};
    std::vector<json> clientResponses;
    std::vector<json> exchangeRequests;

    void SetUp() override {
        gateway.setSendToClient(
            [this](const json &resp) { clientResponses.push_back(resp); });

        gateway.setSendToExchange([this](const json &req) {
            exchangeRequests.push_back(req);
            if (req.contains("origClOrderId")) {
                exchange.handleCancel(req);
            } else {
                exchange.handleOrder(req);
            }
        });

        exchange.setSendToClient(
            [this](const json &resp) { gateway.handleResponse(resp); });
    }
};

// ─── 测试基类：纯撮合模式 (exchange only) ──────────────────────────

class RequirementExchangeTest : public testing::Test {
  protected:
    TradeSystem system{1};
    std::vector<json> clientResponses;

    void SetUp() override {
        system.setSendToClient(
            [this](const json &resp) { clientResponses.push_back(resp); });
        // 不设置 sendToExchange → 纯撮合模式
    }
};

// ════════════════════════════════════════════════════════════════════
// 2.1 基础目标
// ════════════════════════════════════════════════════════════════════

// ────────────────────────────────────────────────────────────────────
// 2.1.1 交易转发
// ────────────────────────────────────────────────────────────────────

// 2.1.1.1 实现模拟交易转发逻辑，接受输入交易订单，输出交易订单
TEST_F(RequirementGatewayTest, R2_1_1_1_OrderForwarding_Buy) {
    // 输入一个买单，应通过 sendToExchange 转发给交易所
    gateway.handleOrder(
        makeOrder("O1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    // 验证转发给交易所的请求
    ASSERT_GE(exchangeRequests.size(), 1u);
    auto &req = exchangeRequests[0];
    EXPECT_EQ(req["clOrderId"], "O1");
    EXPECT_EQ(req["market"], "XSHG");
    EXPECT_EQ(req["securityId"], "600030");
    EXPECT_EQ(req["side"], "B");
    EXPECT_DOUBLE_EQ(req["price"].get<double>(), 10.0);
    EXPECT_EQ(req["qty"], 100);
    EXPECT_EQ(req["shareholderId"], "SH001");
}

TEST_F(RequirementGatewayTest, R2_1_1_1_OrderForwarding_Sell) {
    // 输入一个卖单，应通过 sendToExchange 转发给交易所
    gateway.handleOrder(
        makeOrder("O2", "XSHE", "000001", "S", 20.0, 200, "SZ001"));

    ASSERT_GE(exchangeRequests.size(), 1u);
    auto &req = exchangeRequests[0];
    EXPECT_EQ(req["clOrderId"], "O2");
    EXPECT_EQ(req["side"], "S");
    EXPECT_EQ(req["qty"], 200);
}

// 2.1.1.2 实现模拟回报转发逻辑，接受输入回报，输出回报
TEST_F(RequirementGatewayTest, R2_1_1_2_ResponseForwarding_Confirm) {
    // 订单 → 交易所确认 → 确认回报应转发给客户端
    gateway.handleOrder(
        makeOrder("O1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    // 交易所确认回报被转发给客户端
    ASSERT_GE(clientResponses.size(), 1u);
    auto &resp = clientResponses[0];
    EXPECT_EQ(resp["clOrderId"], "O1");
    EXPECT_EQ(resp["qty"], 100);
    EXPECT_FALSE(resp.contains("execId")); // 确认回报，不含成交信息
}

TEST_F(RequirementGatewayTest, R2_1_1_2_ResponseForwarding_Execution) {
    // 卖方挂簿 → 买方撮合 → 交易所内成交 → 成交回报应转发给客户端
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    // 应收到成交回报（含 execId）
    bool hasExecReport = false;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            hasExecReport = true;
            EXPECT_EQ(resp["execQty"], 100);
        }
    }
    EXPECT_TRUE(hasExecReport) << "成交回报应触发 sendToClient 回调";
}

// 2.1.1.3 实现基本的交易校验功能，对于简单的无效订单，输出一条交易非法的回报
TEST_F(RequirementGatewayTest, R2_1_1_3_InvalidOrder_MissingFields) {
    // 缺少必要字段的无效订单
    json badOrder = {{"clOrderId", "BAD1"}, {"market", "INVALID"}};
    gateway.handleOrder(badOrder);

    ASSERT_EQ(clientResponses.size(), 1u);
    EXPECT_EQ(clientResponses[0]["rejectCode"],
              ORDER_INVALID_FORMAT_REJECT_CODE);
    EXPECT_TRUE(clientResponses[0].contains("rejectText"));
}

TEST_F(RequirementGatewayTest, R2_1_1_3_InvalidOrder_ZeroQty) {
    // qty=0 的无效订单
    json badOrder = makeOrder("BAD2", "XSHG", "600030", "B", 10.0, 0, "SH001");
    gateway.handleOrder(badOrder);

    ASSERT_EQ(clientResponses.size(), 1u);
    EXPECT_EQ(clientResponses[0]["rejectCode"],
              ORDER_INVALID_FORMAT_REJECT_CODE);
}

TEST_F(RequirementGatewayTest, R2_1_1_3_InvalidOrder_NegativePrice) {
    // 负价格的无效订单
    json badOrder =
        makeOrder("BAD3", "XSHG", "600030", "B", -5.0, 100, "SH001");
    gateway.handleOrder(badOrder);

    ASSERT_EQ(clientResponses.size(), 1u);
    EXPECT_EQ(clientResponses[0]["rejectCode"],
              ORDER_INVALID_FORMAT_REJECT_CODE);
}

TEST_F(RequirementGatewayTest, R2_1_1_3_InvalidCancel) {
    // 缺少必要字段的撤单请求
    json badCancel = {{"clOrderId", "C1"}};
    gateway.handleCancel(badCancel);

    ASSERT_EQ(clientResponses.size(), 1u);
    EXPECT_EQ(clientResponses[0]["rejectCode"],
              ORDER_INVALID_FORMAT_REJECT_CODE);
}

// ────────────────────────────────────────────────────────────────────
// 2.1.2 对敲风控
// ────────────────────────────────────────────────────────────────────

// 2.1.2.1 实现对于会触发对敲交易的交易订单的过滤，不输出该交易订单
TEST_F(RequirementGatewayTest, R2_1_2_1_CrossTrade_NotForwarded) {
    // 同一股东先买后卖 → 卖单不应转发给交易所
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    exchangeRequests.clear();

    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH001"));

    // 对敲订单不应出现在转发队列中
    for (const auto &req : exchangeRequests) {
        EXPECT_NE(req["clOrderId"], "S1") << "对敲订单不应转发给交易所";
    }
}

TEST_F(RequirementGatewayTest, R2_1_2_1_CrossTrade_SameShareholderSellFirst) {
    // 同一股东先卖后买 → 买单不应转发给交易所
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH001"));
    exchangeRequests.clear();

    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    for (const auto &req : exchangeRequests) {
        EXPECT_NE(req["clOrderId"], "B1") << "对敲订单不应转发给交易所";
    }
}

// 2.1.2.2 实现对于会触发对敲交易的交易订单，输出一条交易非法的回报
TEST_F(RequirementGatewayTest, R2_1_2_2_CrossTrade_RejectReport) {
    // 同一股东先买后卖 → 应输出对敲拒绝回报
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH001"));

    ASSERT_EQ(clientResponses.size(), 1u);
    EXPECT_EQ(clientResponses[0]["clOrderId"], "S1");
    EXPECT_EQ(clientResponses[0]["rejectCode"], ORDER_CROSS_TRADE_REJECT_CODE);
    EXPECT_TRUE(clientResponses[0].contains("rejectText"));
}

TEST_F(RequirementGatewayTest,
       R2_1_2_2_CrossTrade_DifferentShareholder_NoReject) {
    // 不同股东号先买后卖 → 不应对敲拒绝
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));

    // 不应有拒绝回报
    for (const auto &resp : clientResponses) {
        EXPECT_FALSE(resp.contains("rejectCode"))
            << "不同股东号不应触发对敲检测";
    }
}

TEST_F(RequirementGatewayTest, R2_1_2_2_CrossTrade_SameDirection_NoReject) {
    // 同一股东同方向 → 不应对敲拒绝
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("B2", "XSHG", "600030", "B", 10.5, 200, "SH001"));

    for (const auto &resp : clientResponses) {
        EXPECT_FALSE(resp.contains("rejectCode"))
            << "同方向订单不应触发对敲检测";
    }
}

TEST_F(RequirementGatewayTest, R2_1_2_2_CrossTrade_DifferentSecurity_NoReject) {
    // 同一股东不同股票 → 不应对敲拒绝
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("S1", "XSHG", "601318", "S", 50.0, 100, "SH001"));

    for (const auto &resp : clientResponses) {
        EXPECT_FALSE(resp.contains("rejectCode")) << "不同股票不应触发对敲检测";
    }
}

// ────────────────────────────────────────────────────────────────────
// 2.1.3 模拟撮合
// ────────────────────────────────────────────────────────────────────

// 2.1.3.1 实现对敲交易订端自动撮合，返回成交回报
TEST_F(RequirementGatewayTest, R2_1_3_1_InternalMatch_ExecReport) {
    // 卖方挂簿 → 买方来 → 内部自动撮合 → 返回成交回报
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    // 应有成交回报（买方+卖方各一条）
    int execCount = 0;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            execCount++;
            EXPECT_EQ(resp["execQty"], 100);
        }
    }
    EXPECT_EQ(execCount, 2) << "应产生被动方和主动方各一条成交回报";
}

TEST_F(RequirementGatewayTest, R2_1_3_1_InternalMatch_ExecId_Consistent) {
    // 两条成交回报的 execId 应相同
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    std::string execId;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            if (execId.empty()) {
                execId = resp["execId"].get<std::string>();
            } else {
                EXPECT_EQ(resp["execId"], execId)
                    << "同一笔成交的两条回报应有相同的 execId";
            }
        }
    }
    EXPECT_FALSE(execId.empty());
}

// 2.1.3.2 被匹配的对手方交易订单需要从交易所侧撤回
TEST_F(RequirementGatewayTest, R2_1_3_2_CounterpartyCancel_Sent) {
    // 卖方已转发到交易所 → 买方内部撮合 → 应向交易所发送撤单请求
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    exchangeRequests.clear();

    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    // 检查是否发送了针对 S1 的撤单请求
    bool cancelSent = false;
    for (const auto &req : exchangeRequests) {
        if (req.contains("origClOrderId") && req["origClOrderId"] == "S1") {
            cancelSent = true;
        }
    }
    EXPECT_TRUE(cancelSent) << "内部撮合后应向交易所发送对手方撤单请求";
}

TEST_F(RequirementGatewayTest, R2_1_3_2_MultipleCounterparties_AllCanceled) {
    // 多个对手方都应被撤单
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    gateway.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 10.0, 200, "SH003"));
    exchangeRequests.clear();

    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 300, "SH001"));

    int cancelCount = 0;
    for (const auto &req : exchangeRequests) {
        if (req.contains("origClOrderId")) {
            cancelCount++;
        }
    }
    EXPECT_EQ(cancelCount, 2) << "两个对手方都应被撤单";
}

// 2.1.3.3 设计成交价生成算法（maker price）
TEST_F(RequirementGatewayTest, R2_1_3_3_ExecPrice_MakerPrice_BuyAggressor) {
    // 卖方（maker）挂 9.5，买方（taker）出 10.0 → 成交价应为 9.5
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 9.5, 100, "SH002"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            EXPECT_DOUBLE_EQ(resp["execPrice"].get<double>(), 9.5)
                << "成交价应为被动方（maker）的挂单价";
        }
    }
}

TEST_F(RequirementGatewayTest, R2_1_3_3_ExecPrice_MakerPrice_SellAggressor) {
    // 买方（maker）挂 10.5，卖方（taker）出 10.0 → 成交价应为 10.5
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.5, 100, "SH001"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));

    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            EXPECT_DOUBLE_EQ(resp["execPrice"].get<double>(), 10.5)
                << "成交价应为被动方（maker）的挂单价";
        }
    }
}

TEST_F(RequirementExchangeTest, R2_1_3_3_ExecPrice_PricePriority) {
    // 价格优先：先匹配更优价格
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 11.0, 100, "SH002"));
    system.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 10.0, 100, "SH003"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 11.0, 100, "SH001"));

    // 应优先匹配 10.0 的卖单
    bool matchedCheaper = false;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId") && resp["clOrderId"] == "S2") {
            matchedCheaper = true;
            EXPECT_DOUBLE_EQ(resp["execPrice"].get<double>(), 10.0);
        }
    }
    EXPECT_TRUE(matchedCheaper) << "应价格优先匹配更便宜的卖单";
}

TEST_F(RequirementExchangeTest, R2_1_3_3_ExecPrice_TimePriority) {
    // 同价时间优先：先挂单的先成交
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    system.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 10.0, 100, "SH003"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    ASSERT_GE(clientResponses.size(), 3u);
    // 被动方成交回报应为 S1（先挂单的）
    EXPECT_EQ(clientResponses[1]["clOrderId"], "S1")
        << "同价位应按时间优先匹配";
}

// 2.1.3.4 处理零股成交
TEST_F(RequirementExchangeTest, R2_1_3_4_OddLot_SellLessThan100) {
    // 买方100股挂簿，卖方50股（零股）来 → 应能成交
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 50, "SH002"));

    int execCount = 0;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            execCount++;
            EXPECT_EQ(resp["execQty"], 50);
        }
    }
    EXPECT_EQ(execCount, 2) << "零股卖单应能正常成交";
}

TEST_F(RequirementExchangeTest, R2_1_3_4_OddLot_Sell150) {
    // 买方200股挂簿，卖方150股来 → 应能成交150股
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 200, "SH001"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 150, "SH002"));

    bool found = false;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            found = true;
            EXPECT_EQ(resp["execQty"], 150);
        }
    }
    EXPECT_TRUE(found) << "150股（非100的倍数）卖单应能成交";
}

TEST_F(RequirementGatewayTest, R2_1_3_4_OddLot_GatewayMode) {
    // 前置模式下零股同样可以成交
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 50, "SH002"));

    bool found = false;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            found = true;
            EXPECT_EQ(resp["execQty"], 50);
        }
    }
    EXPECT_TRUE(found) << "前置模式下零股卖单应能成交";
}

// ════════════════════════════════════════════════════════════════════
// 2.2 高级目标
// ════════════════════════════════════════════════════════════════════

// ────────────────────────────────────────────────────────────────────
// 2.2.1 行情接入
// ────────────────────────────────────────────────────────────────────

// 2.2.1.1 考虑行情信息：能读取解析输入的行情信息
TEST_F(RequirementGatewayTest, R2_2_1_1_MarketData_CanReceive) {
    // 系统可以接收行情数据而不崩溃
    json marketData = {{"market", "XSHG"},
                       {"securityId", "600030"},
                       {"bidPrice", 9.8},
                       {"askPrice", 10.2}};

    // handleMarketData 不应抛异常
    EXPECT_NO_THROW(gateway.handleMarketData(marketData));
}

TEST_F(RequirementGatewayTest, R2_2_1_1_MarketData_ViaExchangePush) {
    // 通过交易所行情推送链路接收行情
    std::vector<json> receivedMarketData;
    exchange.setSendMarketData(
        [&](const json &data) { receivedMarketData.push_back(data); });

    // 挂单触发行情更新
    exchange.handleOrder(
        makeOrder("MD1", "XSHG", "600030", "S", 10.0, 100, "MD001"));

    // 交易所应通过 sendMarketData 推送行情
    EXPECT_GE(receivedMarketData.size(), 1u) << "挂单后应推送行情数据";
}

// 2.2.1.2 在撮合时考虑行情信息：撮合时需要保证买价、卖价和对手方价格保持一致
TEST_F(RequirementGatewayTest, R2_2_1_2_MarketDataConstraint_BuySide) {
    // 场景：行情卖价 9.5，内部卖盘 9.0 和 10.0
    // 买单 10.0 → 应先吃 9.0 档，10.0 档被行情约束阻止
    exchange.setSendMarketData(
        [this](const json &data) { gateway.handleMarketData(data); });

    // 构造行情：交易所挂单形成行情 → bestAsk=9.5
    exchange.handleOrder(
        makeOrder("MD1", "XSHG", "600030", "S", 9.5, 100, "MD001"));
    exchange.handleOrder(
        makeOrder("MD2", "XSHG", "600030", "B", 8.5, 100, "MD002"));

    // 前置系统挂两档卖单
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 9.0, 100, "SH002"));
    gateway.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 10.0, 100, "SH003"));
    clientResponses.clear();

    // 买单 → 9.0 档内部成交，10.0 档被行情约束阻止后送交易所
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 200, "SH001"));

    // 应有成交回报
    bool foundS1Exec = false;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId") && resp["clOrderId"] == "S1") {
            foundS1Exec = true;
            EXPECT_DOUBLE_EQ(resp["execPrice"].get<double>(), 9.0);
        }
    }
    EXPECT_TRUE(foundS1Exec) << "低于行情卖价的内部卖单应正常成交";

    // S2 (10.0 > 行情卖价 9.5) 不应在内部成交
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId") && resp["clOrderId"] == "S2") {
            FAIL() << "10.0 的内部卖单应被行情约束阻止，不应在内部成交";
        }
    }
}

TEST_F(RequirementGatewayTest, R2_2_1_2_MarketDataConstraint_SellSide) {
    // 场景：行情买价 9.5，内部买盘 9.0 和 10.0
    // 卖单 9.0 → 应先吃 10.0 档，9.0 档被行情约束阻止
    exchange.setSendMarketData(
        [this](const json &data) { gateway.handleMarketData(data); });

    // 构造行情：交易所挂单形成行情 → bestBid=9.5
    exchange.handleOrder(
        makeOrder("MD1", "XSHG", "600030", "B", 9.5, 100, "MD001"));
    exchange.handleOrder(
        makeOrder("MD2", "XSHG", "600030", "S", 10.5, 100, "MD002"));

    // 前置系统挂两档买单
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 9.0, 100, "SH001"));
    gateway.handleOrder(
        makeOrder("B2", "XSHG", "600030", "B", 10.0, 100, "SH002"));
    clientResponses.clear();

    // 卖单 → 10.0 档内部成交，9.0 档被行情约束阻止后送交易所
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 9.0, 200, "SH003"));

    // B2 (10.0 > 行情买价 9.5) 应正常成交
    bool foundB2Exec = false;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId") && resp["clOrderId"] == "B2") {
            foundB2Exec = true;
            EXPECT_DOUBLE_EQ(resp["execPrice"].get<double>(), 10.0);
        }
    }
    EXPECT_TRUE(foundB2Exec) << "高于行情买价的内部买单应正常成交";

    // B1 (9.0 < 行情买价 9.5) 不应在内部成交
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId") && resp["clOrderId"] == "B1") {
            FAIL() << "9.0 的内部买单应被行情约束阻止，不应在内部成交";
        }
    }
}

// ────────────────────────────────────────────────────────────────────
// 2.2.2 撤单支持
// ────────────────────────────────────────────────────────────────────

// 2.2.2.1 处理输入的撤单请求 — 基本撤单
TEST_F(RequirementGatewayTest, R2_2_2_1_Cancel_BasicForward) {
    // 挂单后用户撤单 → 撤单请求转发给交易所 → 确认回报转发给客户端
    gateway.handleOrder(
        makeOrder("O1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();
    exchangeRequests.clear();

    gateway.handleCancel(
        makeCancel("C1", "O1", "XSHG", "600030", "SH001", "B"));

    // 撤单请求应转发给交易所
    bool cancelForwarded = false;
    for (const auto &req : exchangeRequests) {
        if (req.contains("origClOrderId") && req["origClOrderId"] == "O1") {
            cancelForwarded = true;
        }
    }
    EXPECT_TRUE(cancelForwarded) << "撤单请求应转发给交易所";

    // 客户端应收到撤单确认回报
    ASSERT_GE(clientResponses.size(), 1u);
    bool cancelConfirmed = false;
    for (const auto &resp : clientResponses) {
        if (resp.contains("origClOrderId") && resp["origClOrderId"] == "O1") {
            cancelConfirmed = true;
            EXPECT_EQ(resp["canceledQty"], 100);
        }
    }
    EXPECT_TRUE(cancelConfirmed) << "客户端应收到撤单确认回报";
}

// 2.2.2.1 撤单后订单不可再匹配
TEST_F(RequirementGatewayTest, R2_2_2_1_Cancel_NoMatchAfterCancel) {
    // 挂卖单 → 撤单 → 买单来了不应匹配
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    gateway.handleCancel(
        makeCancel("C1", "S1", "XSHG", "600030", "SH002", "S"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    for (const auto &resp : clientResponses) {
        EXPECT_FALSE(resp.contains("execId")) << "已撤单的订单不应参与匹配";
    }
}

// 2.2.2.1 纯撮合模式下的撤单
TEST_F(RequirementExchangeTest, R2_2_2_1_Cancel_ExchangeMode) {
    // 纯撮合模式下的撤单
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    system.handleCancel(makeCancel("C1", "B1", "XSHG", "600030", "SH001", "B"));

    ASSERT_EQ(clientResponses.size(), 1u);
    EXPECT_EQ(clientResponses[0]["origClOrderId"], "B1");
    EXPECT_EQ(clientResponses[0]["canceledQty"], 100);
    EXPECT_EQ(clientResponses[0]["cumQty"], 0);
}

// 2.2.2.1 部分成交后撤单，返回正确的已成交和撤销数量
TEST_F(RequirementExchangeTest, R2_2_2_1_Cancel_AfterPartialExec) {
    // 买方200挂簿 → 卖方150成交 → 撤剩余50
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 200, "SH001"));
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 150, "SH002"));
    clientResponses.clear();

    system.handleCancel(makeCancel("C1", "B1", "XSHG", "600030", "SH001", "B"));

    ASSERT_EQ(clientResponses.size(), 1u);
    auto &resp = clientResponses[0];
    EXPECT_EQ(resp["origClOrderId"], "B1");
    EXPECT_EQ(resp["canceledQty"], 50);
    EXPECT_EQ(resp["cumQty"], 150);
    EXPECT_EQ(resp["qty"], 200);
}

// 2.2.2.1 撤单后对敲状态应更新
TEST_F(RequirementExchangeTest, R2_2_2_1_Cancel_CrossTradeStateUpdated) {
    // 买单 → 撤单 → 同股东卖单不应触发对敲
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    system.handleCancel(makeCancel("C1", "B1", "XSHG", "600030", "SH001", "B"));
    clientResponses.clear();

    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH001"));

    ASSERT_EQ(clientResponses.size(), 1u);
    EXPECT_FALSE(clientResponses[0].contains("rejectCode"))
        << "撤单后同股东反向订单不应触发对敲检测";
}
