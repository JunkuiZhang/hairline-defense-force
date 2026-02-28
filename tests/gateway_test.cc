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

// ================================================================
// 交易所前置模式测试基类
// gateway（前置系统）←→ exchange（纯撮合系统充当交易所）
//
// 数据流：
//   客户端 → gateway.handleOrder/handleCancel
//   gateway → exchange.handleOrder/handleCancel  (sendToExchange_)
//   exchange → gateway.handleResponse            (exchange的sendToClient_)
//   gateway → clientResponses                    (gateway的sendToClient_)
// ================================================================
class GatewayTest : public testing::Test {
  protected:
    TradeSystem gateway;               // 交易所前置
    TradeSystem exchange;              // 纯撮合系统充当交易所
    std::vector<json> clientResponses; // 最终发给客户端的回报

    void SetUp() override {
        // 客户端回报
        gateway.setSendToClient(
            [this](const json &resp) { clientResponses.push_back(resp); });

        // 前置 → 交易所：根据是否含 origClOrderId 区分订单/撤单
        gateway.setSendToExchange([this](const json &req) {
            if (req.contains("origClOrderId")) {
                exchange.handleCancel(req);
            } else {
                exchange.handleOrder(req);
            }
        });

        // 交易所 → 前置：交易所的"客户端"就是前置
        exchange.setSendToClient(
            [this](const json &resp) { gateway.handleResponse(resp); });

        // exchange 不设置 sendToExchange_ → 纯撮合模式
    }
};

// 用于模拟交易所拒绝的情况
class PureGatewayTest : public testing::Test {
  protected:
    TradeSystem system;                 // 交易所前置
    std::vector<json> clientResponses;  // 最终发给客户端的回报
    std::vector<json> exchangeRequests; // gateway转发给交易所的请求

    void SetUp() override {
        system.setSendToClient(
            [this](const json &resp) { clientResponses.push_back(resp); });
        system.setSendToExchange(
            [this](const json &req) { exchangeRequests.push_back(req); });
    }
};

// ==================== 无匹配 → 转发交易所 → 交易所确认 ====================

TEST_F(GatewayTest, NoMatch_ExchangeConfirmForwarded) {
    // 单个买单无对手方 → 入内部簿 + 转发交易所 → 交易所确认 → 客户端收到确认
    gateway.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    // 交易所确认回报应被转发到客户端
    ASSERT_EQ(clientResponses.size(), 1);

    auto &resp = clientResponses[0];
    EXPECT_EQ(resp["clOrderId"], "1001");
    EXPECT_EQ(resp["market"], "XSHG");
    EXPECT_EQ(resp["securityId"], "600030");
    EXPECT_EQ(resp["side"], "B");
    EXPECT_EQ(resp["qty"], 100);
    EXPECT_DOUBLE_EQ(resp["price"].get<double>(), 10.0);
    // 确认回报不含 execId
    EXPECT_FALSE(resp.contains("execId"));
}

TEST_F(GatewayTest, NoMatch_SellConfirmForwarded) {
    gateway.handleOrder(
        makeOrder("2001", "XSHE", "000001", "S", 20.0, 50, "SZ001"));

    // 交易所确认回报应被转发到客户端
    ASSERT_EQ(clientResponses.size(), 1);

    auto &resp = clientResponses[0];
    EXPECT_EQ(resp["clOrderId"], "2001");
    EXPECT_EQ(resp["market"], "XSHE");
    EXPECT_EQ(resp["securityId"], "000001");
    EXPECT_EQ(resp["side"], "S");
    EXPECT_EQ(resp["qty"], 50);
    // 确认回报不含 execId
    EXPECT_FALSE(resp.contains("execId"));
}

// ==================== 内部撮合 → 撤单 → 成交回报 ====================

TEST_F(GatewayTest, InternalMatch_FullFlow) {
    // 1. 卖单先到：无匹配 → 转发交易所 → 确认
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    ASSERT_EQ(clientResponses.size(), 1); // 交易所确认(S1)
    clientResponses.clear();

    // 2. 买单来了 → 内部撮合成功 → 向交易所发撤单 → 交易所撤单确认
    //    → resolvePendingMatch → 成交回报
    //    整个流程在同步调用中完成
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    // 客户端应收到：确认回报(B1) + 被动方成交(S1) + 主动方成交(B1)
    // 确认回报在成交回报之前
    ASSERT_EQ(clientResponses.size(), 3);

    // 确认回报
    EXPECT_EQ(clientResponses[0]["clOrderId"], "B1");
    EXPECT_EQ(clientResponses[0]["qty"], 100);
    EXPECT_FALSE(clientResponses[0].contains("execId"));

    // 被动方成交回报
    EXPECT_EQ(clientResponses[1]["clOrderId"], "S1");
    EXPECT_EQ(clientResponses[1]["side"], "S");
    EXPECT_TRUE(clientResponses[1].contains("execId"));
    EXPECT_EQ(clientResponses[1]["execQty"], 100);
    EXPECT_DOUBLE_EQ(clientResponses[1]["execPrice"].get<double>(), 10.0);

    // 主动方成交回报
    EXPECT_EQ(clientResponses[2]["clOrderId"], "B1");
    EXPECT_EQ(clientResponses[2]["side"], "B");
    EXPECT_EQ(clientResponses[2]["execQty"], 100);

    // 两个成交回报的 execId 应相同
    EXPECT_EQ(clientResponses[1]["execId"], clientResponses[2]["execId"]);
}

TEST_F(GatewayTest, InternalMatch_MultipleCounterparties) {
    // 三个卖单先到（各自获得交易所确认）
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 200, "SH002"));
    gateway.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 10.0, 300, "SH003"));
    gateway.handleOrder(
        makeOrder("S3", "XSHG", "600030", "S", 10.5, 500, "SH004"));
    EXPECT_EQ(clientResponses.size(), 3); // 3个确认
    clientResponses.clear();

    // 大买单匹配3个卖单 → 3个撤单 → 全部确认 → 成交回报
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.5, 1000, "SH001"));

    // 3笔成交×2（被动+主动）= 6 + 1确认 = 7
    ASSERT_EQ(clientResponses.size(), 7);

    // 确认回报
    EXPECT_EQ(clientResponses[0]["clOrderId"], "B1");
    EXPECT_EQ(clientResponses[0]["qty"], 1000);
    EXPECT_FALSE(clientResponses[0].contains("execId"));

    // 第1笔成交（S1, 价格优先 10.0）
    EXPECT_EQ(clientResponses[1]["clOrderId"], "S1");
    EXPECT_EQ(clientResponses[1]["execQty"], 200);
    EXPECT_DOUBLE_EQ(clientResponses[1]["execPrice"].get<double>(), 10.0);
    EXPECT_EQ(clientResponses[2]["clOrderId"], "B1");
    EXPECT_EQ(clientResponses[2]["execQty"], 200);

    // 第2笔成交（S2, 同价时间优先）
    EXPECT_EQ(clientResponses[3]["clOrderId"], "S2");
    EXPECT_EQ(clientResponses[3]["execQty"], 300);
    EXPECT_DOUBLE_EQ(clientResponses[3]["execPrice"].get<double>(), 10.0);

    // 第3笔成交（S3, 价格 10.5）
    EXPECT_EQ(clientResponses[5]["clOrderId"], "S3");
    EXPECT_EQ(clientResponses[5]["execQty"], 500);
    EXPECT_DOUBLE_EQ(clientResponses[5]["execPrice"].get<double>(), 10.5);
}

// ==================== 部分成交 → 剩余转发交易所 ====================

TEST_F(GatewayTest, PartialMatch_RemainingForwardedToExchange) {
    // 卖100挂簿
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    EXPECT_EQ(clientResponses.size(), 1); // 卖单确认回报
    clientResponses.clear();

    // 买500来撮合 → 内部成交100 + 剩余400转发交易所
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 500, "SH001"));

    // resolvePendingMatch: 成交记录暂存，剩余400转发交易所
    // 交易所确认后 → 发送确认回报(B1, qty=500) + 成交回报(S1×2)
    ASSERT_EQ(clientResponses.size(), 3);

    // 确认回报在最前（原始订单数量500）
    auto &first = clientResponses[0];
    EXPECT_EQ(first["clOrderId"], "B1");
    EXPECT_EQ(first["qty"], 500);
    EXPECT_FALSE(first.contains("execId"));

    // 找成交回报
    int execReportCount = 0;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            execReportCount++;
            EXPECT_EQ(resp["execQty"], 100);
        }
    }
    EXPECT_EQ(execReportCount, 2); // 被动方 + 主动方
}

// ==================== 零股 ====================

TEST_F(GatewayTest, OddLot_SellCanBeOddLot) {
    // 买方100股挂簿，卖方50股来 → 应能成交（卖方可零股）
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    ASSERT_EQ(clientResponses.size(), 1); // 买单确认回报
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 50, "SH002"));

    // 1个确认回报 + 2个成交回报 = 3
    ASSERT_GE(clientResponses.size(), 3);
    // 应成交50股
    EXPECT_EQ(clientResponses[1]["execQty"], 50);
}

TEST_F(GatewayTest, OddLot_SellCanBeOddLot_2) {
    // 买方200股挂簿，卖方150股来 → 应能成交（卖方可零股）
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 200, "SH001"));
    ASSERT_EQ(clientResponses.size(), 1); // 买单确认回报
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 150, "SH002"));

    // 1个确认回报 + 2个成交回报 = 3
    ASSERT_GE(clientResponses.size(), 3);
    // 应成交150股
    EXPECT_EQ(clientResponses[1]["execQty"], 150);
}

TEST_F(GatewayTest, OddLot_SellCanBeOddLot_with_Cancel) {
    // 买方200股挂簿，卖方150股来 → 应能成交（卖方可零股）
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 200, "SH001"));
    ASSERT_EQ(clientResponses.size(), 1); // 买单确认回报
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 150, "SH002"));

    // 1个确认回报 + 2个成交回报 = 3
    ASSERT_GE(clientResponses.size(), 3);
    // 应成交150股
    EXPECT_EQ(clientResponses[1]["execQty"], 150);
    clientResponses.clear();

    // 买方剩余50股，然后买方撤单
    gateway.handleCancel(
        makeCancel("C1", "B1", "XSHG", "600030", "SH001", "B"));
    ASSERT_EQ(clientResponses.size(), 1); // 撤单确认回报
    auto &resp = clientResponses[0];
    EXPECT_EQ(resp["origClOrderId"], "B1");
    EXPECT_EQ(resp["canceledQty"], 50);
    EXPECT_EQ(resp["cumQty"], 150);
    EXPECT_EQ(resp["qty"], 200);
}

// ==================== 对敲检测（前置模式同样生效） ====================

TEST_F(GatewayTest, CrossTrade_Rejected) {
    // 同一股东号先买后卖 → 对敲拒绝
    gateway.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("2001", "XSHG", "600030", "S", 10.0, 100, "SH001"));

    // 对敲拒绝回报
    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["clOrderId"], "2001");
    EXPECT_EQ(clientResponses[0]["rejectCode"], ORDER_CROSS_TRADE_REJECT_CODE);
}

TEST_F(GatewayTest, InternalMatch_CrossTradeStillBlocked) {
    // SH001 卖单入簿 → SH001 买单 → 对敲拒绝
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH001"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["rejectCode"], ORDER_CROSS_TRADE_REJECT_CODE);
}

// ==================== JSON 解析错误 ====================

TEST_F(GatewayTest, InvalidOrderJson_Rejected) {
    json badOrder = {{"clOrderId", "BAD1"}, {"market", "INVALID"}};
    gateway.handleOrder(badOrder);

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["rejectCode"],
              ORDER_INVALID_FORMAT_REJECT_CODE);
}

TEST_F(GatewayTest, InvalidCancelJson_Rejected) {
    json badCancel = {{"clOrderId", "C1"}};
    gateway.handleCancel(badCancel);

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["rejectCode"],
              ORDER_INVALID_FORMAT_REJECT_CODE);
}

// ==================== 撤单转发 ====================

TEST_F(GatewayTest, UserCancel_ForwardedAndConfirmed) {
    // 挂单 → 用户撤单 → 转发交易所 → 交易所撤单确认 → 转发客户端
    gateway.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    ASSERT_EQ(clientResponses.size(), 1); // 买单确认回报
    clientResponses.clear();

    gateway.handleCancel(
        makeCancel("C001", "1001", "XSHG", "600030", "SH001", "B"));

    // 交易所撤单确认应转发到客户端
    ASSERT_EQ(clientResponses.size(), 1);
    auto &resp = clientResponses[0];
    EXPECT_EQ(resp["origClOrderId"], "1001");
    EXPECT_EQ(resp["canceledQty"], 100);
    EXPECT_EQ(resp["cumQty"], 0);
}

// ==================== 成交价（maker price） ====================

TEST_F(GatewayTest, MakerPrice) {
    // 卖方挂 9.5
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 9.5, 100, "SH002"));
    ASSERT_EQ(clientResponses.size(), 1); // 卖单确认回报
    clientResponses.clear();

    // 买方出 10.0 → 成交价应为卖方挂单价 9.5
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    // 找成交回报中的 execPrice
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            EXPECT_DOUBLE_EQ(resp["execPrice"].get<double>(), 9.5);
        }
    }
}

// ==================== 价格优先 ====================

TEST_F(GatewayTest, PricePriority) {
    // 挂两个卖单：贵(11.0)→便宜(10.0)
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 11.0, 100, "SH002"));
    gateway.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 10.0, 100, "SH003"));
    clientResponses.clear();

    // 买单 11.0，应优先匹配便宜的 S2
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 11.0, 100, "SH001"));

    // 找成交回报中的被动方
    bool matchedS2 = false;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId") && resp["clOrderId"] == "S2") {
            matchedS2 = true;
            EXPECT_DOUBLE_EQ(resp["execPrice"].get<double>(), 10.0);
        }
    }
    EXPECT_TRUE(matchedS2);
}

// ==================== 交易所成交 → 内部簿同步 ====================

TEST_F(GatewayTest, ExchangeExecReport_SyncsInternalBook) {
    // 买单入内部簿（无匹配，转发交易所）
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 200, "SH001"));
    ASSERT_EQ(clientResponses.size(), 1); // 买单确认回报
    clientResponses.clear();

    // 模拟交易所侧的成交（其他网关的卖单直接到交易所匹配了 B1）
    // 交易所的成交回报会通过 exchange.sendToClient_ → gateway.handleResponse
    //
    // 由于 exchange 是纯撮合系统，我们直接注入一个卖单给交易所：
    exchange.handleOrder(
        makeOrder("EXT_S1", "XSHG", "600030", "S", 10.0, 100, "EXT_SH"));

    // 交易所匹配了 B1(200) 和 EXT_S1(100) → 成交100
    // 成交回报流回 gateway → 转发客户端 + 更新内部簿
    // 客户端应收到成交回报
    int execCount = 0;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId") && resp["clOrderId"] == "B1") {
            execCount++;
            EXPECT_EQ(resp["execQty"], 100);
        }
    }
    EXPECT_GE(execCount, 1);

    clientResponses.clear();

    // 内部簿应只剩100股
    // 新卖单来验证：内部应只匹配100
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 200, "SH002"));

    // 找内部成交回报
    int internalExecCount = 0;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            internalExecCount++;
            EXPECT_EQ(resp["execQty"], 100);
        }
    }
    EXPECT_EQ(internalExecCount, 2); // 被动方 + 主动方
}

TEST_F(GatewayTest, ExchangeExecReport_FullyFilled_NoInternalMatch) {
    // 买单入簿
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();

    // 交易所侧完全成交（注入卖单到交易所）
    exchange.handleOrder(
        makeOrder("EXT_S1", "XSHG", "600030", "S", 10.0, 100, "EXT_SH"));
    clientResponses.clear();

    // 卖单来了，不应匹配已成交的 B1
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));

    // 无内部匹配：只有交易所确认回报，不含 execId
    bool hasExecReport = false;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId") && resp["clOrderId"] == "S1") {
            hasExecReport = true;
        }
    }
    EXPECT_FALSE(hasExecReport);
}

// ==================== 不同股票不匹配 ====================

TEST_F(GatewayTest, DifferentSecurity_NoInternalMatch) {
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("B1", "XSHG", "601318", "B", 10.0, 100, "SH001"));

    // 不同股票，无内部匹配，只有交易所确认
    ASSERT_GE(clientResponses.size(), 1);
    for (const auto &resp : clientResponses) {
        EXPECT_FALSE(resp.contains("execId"));
    }
}

// ==================== 连续内部撮合 ====================

TEST_F(GatewayTest, SequentialInternalMatches) {
    // 第一轮：S1 → B1 → 内部成交
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    int exec1 = 0;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId"))
            exec1++;
    }
    EXPECT_EQ(exec1, 2); // 被动方+主动方
    clientResponses.clear();

    // 第二轮：S2 → B2 → 内部成交
    gateway.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 11.0, 200, "SH003"));
    clientResponses.clear();

    gateway.handleOrder(
        makeOrder("B2", "XSHG", "600030", "B", 11.0, 200, "SH004"));

    int exec2 = 0;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            exec2++;
            EXPECT_EQ(resp["execQty"], 200);
        }
    }
    EXPECT_EQ(exec2, 2);
}

// ==================== 撤单后不可匹配 ====================

TEST_F(GatewayTest, CancelThenNoInternalMatch) {
    // 卖单入簿
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    ASSERT_EQ(clientResponses.size(), 1); // 交易所确认回报
    clientResponses.clear();

    // 用户撤单（转发交易所 → 交易所撤单 → 确认）
    gateway.handleCancel(
        makeCancel("C001", "S1", "XSHG", "600030", "SH002", "S"));
    ASSERT_EQ(clientResponses.size(), 1); // 撤单确认回报
    clientResponses.clear();

    // 买单来了，不应匹配已撤的 S1
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    // 无成交，只有交易所确认
    ASSERT_EQ(clientResponses.size(), 1);
    for (const auto &resp : clientResponses) {
        EXPECT_FALSE(resp.contains("execId"))
            << "Should not match cancelled order";
    }
}

// ==================== 多个独立 PendingMatch ====================

TEST_F(GatewayTest, MultiplePendingMatches_DifferentSecurities) {
    // 两只股票分别挂卖单
    gateway.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    gateway.handleOrder(
        makeOrder("S2", "XSHE", "000001", "S", 20.0, 200, "SH003"));
    clientResponses.clear();

    // 两只股票分别来买单 → 各自内部匹配
    gateway.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    gateway.handleOrder(
        makeOrder("B2", "XSHE", "000001", "B", 20.0, 200, "SH004"));

    // 验证两只股票的成交都完成了
    bool foundS1Exec = false, foundS2Exec = false;
    bool foundB1Exec = false, foundB2Exec = false;
    for (const auto &resp : clientResponses) {
        if (!resp.contains("execId"))
            continue;
        std::string id = resp["clOrderId"].get<std::string>();
        if (id == "S1")
            foundS1Exec = true;
        if (id == "S2")
            foundS2Exec = true;
        if (id == "B1")
            foundB1Exec = true;
        if (id == "B2")
            foundB2Exec = true;
    }
    EXPECT_TRUE(foundS1Exec);
    EXPECT_TRUE(foundS2Exec);
    EXPECT_TRUE(foundB1Exec);
    EXPECT_TRUE(foundB2Exec);
}

// ==================== PendingMatch: 全部确认 ====================

TEST_F(PureGatewayTest, PendingMatch_AllConfirmed_ExecReportsSent) {
    // 挂卖单（转发交易所+入内部簿）
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    clientResponses.clear();
    exchangeRequests.clear();

    // 买单撮合成功 → 发撤单请求
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    ASSERT_EQ(exchangeRequests.size(), 1);
    clientResponses.clear();
    exchangeRequests.clear();

    // 交易所回报：撤单确认（S1撤单成功）
    json cancelConfirm = {
        {"origClOrderId", "S1"}, {"canceledQty", 100}, {"cumQty", 0}};
    system.handleResponse(cancelConfirm);

    // B1的确认回报 + S1的成交回报 + B1的成交回报
    ASSERT_EQ(clientResponses.size(), 3);

    // 被动方（S1）成交回报
    auto &resp_first = clientResponses[1];
    EXPECT_EQ(resp_first["clOrderId"], "S1");
    EXPECT_EQ(resp_first["side"], "S");
    EXPECT_TRUE(resp_first.contains("execId"));
    EXPECT_EQ(resp_first["execQty"], 100);

    // 主动方（B1）成交回报
    auto &resp_second = clientResponses[2];
    EXPECT_EQ(resp_second["clOrderId"], "B1");
    EXPECT_EQ(resp_second["side"], "B");
    EXPECT_EQ(resp_second["execQty"], 100);

    // 成交回报的 execId 应相同
    EXPECT_EQ(resp_first["execId"], resp_second["execId"]);

    // 不应有新的交易所请求（全部确认，无剩余）
    EXPECT_EQ(exchangeRequests.size(), 0);
}

// ==================== PendingMatch: 全部拒绝 ====================

TEST_F(PureGatewayTest, PendingMatch_AllRejected_RemainingForwarded) {
    // 挂卖单
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));

    // 买单撮合
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();
    exchangeRequests.clear();

    // 交易所回报：撤单被拒（对手方已在交易所被他人成交）
    json cancelReject = {{"origClOrderId", "S1"},
                         {"rejectCode", 99},
                         {"rejectText", "order already filled"}};
    system.handleResponse(cancelReject);

    // 不应有成交回报（撤单被拒表示无法在内部成交）
    // 应把全部数量转发给交易所
    ASSERT_GE(exchangeRequests.size(), 1);
    auto &forwarded = exchangeRequests.back();
    EXPECT_EQ(forwarded["clOrderId"], "B1");
    EXPECT_EQ(forwarded["qty"], 100);

    // 客户端不应收到成交回报
    for (const auto &resp : clientResponses) {
        EXPECT_FALSE(resp.contains("execId"))
            << "Should not send exec report when cancel is rejected";
    }
}

// ==================== PendingMatch: 部分确认部分拒绝 ====================

TEST_F(PureGatewayTest, PendingMatch_PartialConfirm_PartialReject) {
    // 两个卖单挂簿
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 200, "SH002"));
    system.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 10.0, 300, "SH003"));
    clientResponses.clear();
    exchangeRequests.clear();

    // 买单500股，匹配 S1(200) + S2(300)
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 500, "SH001"));
    ASSERT_EQ(exchangeRequests.size(), 2);
    clientResponses.clear();
    exchangeRequests.clear();

    // S1 撤单确认
    json confirmS1 = {
        {"origClOrderId", "S1"}, {"canceledQty", 200}, {"cumQty", 0}};
    system.handleResponse(confirmS1);

    // 还在等S2的回报，不应发成交回报
    EXPECT_EQ(clientResponses.size(), 0);

    // S2 撤单被拒
    json rejectS2 = {{"origClOrderId", "S2"},
                     {"rejectCode", 99},
                     {"rejectText", "order already filled"}};
    system.handleResponse(rejectS2);

    // 现在所有撤单回报都回来了：
    // S1确认 → 成交200（暂存）
    // S2被拒 → 300股未成交，转发交易所
    // 成交回报需等交易所确认剩余300股后才发

    // 被拒部分（300股）应转发给交易所
    ASSERT_GE(exchangeRequests.size(), 1);
    EXPECT_EQ(exchangeRequests.back()["qty"], 300);

    // 此时客户端不应收到成交回报（等交易所确认）
    for (const auto &resp : clientResponses) {
        EXPECT_FALSE(resp.contains("execId"));
    }

    // 模拟交易所确认剩余300股
    clientResponses.clear();
    json exchangeConfirm = {{"clOrderId", "B1"},
                            {"market", "XSHG"},
                            {"securityId", "600030"},
                            {"side", "B"},
                            {"qty", 300},
                            {"price", 10.0},
                            {"shareholderId", "SH001"}};
    system.handleResponse(exchangeConfirm);

    // 交易所确认后：确认回报(B1) + S1成交回报 + B1成交回报
    ASSERT_EQ(clientResponses.size(), 3);

    // 确认回报（原始订单数量500）
    EXPECT_EQ(clientResponses[0]["clOrderId"], "B1");
    EXPECT_EQ(clientResponses[0]["qty"], 500);
    EXPECT_FALSE(clientResponses[0].contains("execId"));

    // 找成交回报（S1 确认的部分）
    int execReportCount = 0;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            execReportCount++;
            EXPECT_EQ(resp["execQty"], 200);
        }
    }
    EXPECT_EQ(execReportCount, 2); // 被动方 + 主动方
}

// ==================== PendingMatch: 撮合有剩余 + 部分拒绝 ====================

TEST_F(PureGatewayTest, PendingMatch_WithRemaining_SomeRejected) {
    // 卖100挂簿
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    exchangeRequests.clear();

    // 买500来撮合 → 成交100 + 剩余400
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 500, "SH001"));
    clientResponses.clear();
    exchangeRequests.clear();

    // S1 撤单被拒
    json rejectS1 = {{"origClOrderId", "S1"},
                     {"rejectCode", 99},
                     {"rejectText", "already filled"}};
    system.handleResponse(rejectS1);

    // 无成交回报
    for (const auto &resp : clientResponses) {
        EXPECT_FALSE(resp.contains("execId"));
    }

    // 被拒100 + 剩余400 = 500 → 全部转发交易所
    ASSERT_GE(exchangeRequests.size(), 1);
    EXPECT_EQ(exchangeRequests.back()["qty"], 500);
}

// ==================== 部分成交 → 剩余转发交易所 ====================

TEST_F(PureGatewayTest, PartialMatch_RemainingForwardedToExchange) {
    // 卖100挂簿
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    ASSERT_EQ(exchangeRequests.size(), 1);
    exchangeRequests.clear();
    // 交易所发送确认回报
    system.handleResponse({{"clOrderId", "S1"},
                           {"market", "XSHG"},
                           {"securityId", "600030"},
                           {"side", "S"},
                           {"qty", 100},
                           {"price", 10.0},
                           {"shareholderId", "SH002"}});
    ASSERT_EQ(clientResponses.size(), 1); // 卖单确认回报
    clientResponses.clear();

    // 买500来撮合 → 内部成交100 + 剩余400转发交易所
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 500, "SH001"));

    // resolvePendingMatch: 成交记录暂存，剩余400转发交易所
    // 交易所确认后 → 发送确认回报(B1, qty=500) + 成交回报(S1×2)

    // 交易所收到撤单请求
    ASSERT_EQ(exchangeRequests.size(), 1);
    ASSERT_EQ(exchangeRequests[0]["origClOrderId"], "S1");
    ASSERT_EQ(exchangeRequests[0]["side"], "S");
    ASSERT_EQ(exchangeRequests[0]["shareholderId"], "SH002");
    exchangeRequests.clear();

    // 交易所发送撤单确认回报
    system.handleResponse({{"clOrderId", "C1"},
                           {"origClOrderId", "S1"},
                           {"market", "XSHG"},
                           {"securityId", "600030"},
                           {"shareholderId", "SH002"},
                           {"side", "S"},
                           {"qty", 100},
                           {"price", 10.0},
                           {"canceledQty", 100},
                           {"cumQty", 0}});
    clientResponses.clear();

    // 交易所收到剩余400的订单
    ASSERT_EQ(exchangeRequests.size(), 1);
    EXPECT_EQ(exchangeRequests[0]["side"], "B");
    EXPECT_EQ(exchangeRequests[0]["shareholderId"], "SH001");
    EXPECT_EQ(exchangeRequests[0]["qty"], 400);
    EXPECT_EQ(exchangeRequests[0]["price"], 10.0);

    system.handleResponse({{"clOrderId", "B1"},
                           {"market", "XSHG"},
                           {"securityId", "600030"},
                           {"side", "B"},
                           {"qty", 400},
                           {"price", 10.0},
                           {"shareholderId", "SH001"}});
    ASSERT_EQ(clientResponses.size(), 3);

    // 确认回报在最前（原始订单数量500）
    auto &first = clientResponses[0];
    EXPECT_EQ(first["clOrderId"], "B1");
    EXPECT_EQ(first["qty"], 500);
    EXPECT_FALSE(first.contains("execId"));

    // 找成交回报
    int execReportCount = 0;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            execReportCount++;
            EXPECT_EQ(resp["execQty"], 100);
        }
    }
    EXPECT_EQ(execReportCount, 2); // 被动方 + 主动方

    // 交易所收到剩余400的订单
    ASSERT_GE(exchangeRequests.size(), 1);
    auto &forwarded = exchangeRequests.back();
    EXPECT_EQ(forwarded["clOrderId"], "B1");
    EXPECT_EQ(forwarded["qty"], 400);
}
