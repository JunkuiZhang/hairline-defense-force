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
// 交易所前置模式测试基类：设置 sendToExchange
// ================================================================
class GatewayTest : public testing::Test {
  protected:
    TradeSystem system;
    std::vector<json> clientResponses;  // op4: 发给客户端的回报
    std::vector<json> exchangeRequests; // op2: 发给交易所的请求

    void SetUp() override {
        system.setSendToClient(
            [this](const json &resp) { clientResponses.push_back(resp); });
        system.setSendToExchange(
            [this](const json &req) { exchangeRequests.push_back(req); });
    }
};

// ==================== 无匹配 → 转发交易所 ====================

TEST_F(GatewayTest, NoMatch_ForwardedToExchange) {
    // 单个买单无对手方 → 入内部簿 + 转发交易所
    system.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    // 不应发给客户端任何回报（无确认回报，等交易所的确认）
    EXPECT_EQ(clientResponses.size(), 0);

    // 应转发给交易所
    ASSERT_EQ(exchangeRequests.size(), 1);
    EXPECT_EQ(exchangeRequests[0]["clOrderId"], "1001");
    EXPECT_EQ(exchangeRequests[0]["market"], "XSHG");
    EXPECT_EQ(exchangeRequests[0]["securityId"], "600030");
    EXPECT_EQ(exchangeRequests[0]["side"], "B");
    EXPECT_EQ(exchangeRequests[0]["qty"], 100);
    EXPECT_DOUBLE_EQ(exchangeRequests[0]["price"].get<double>(), 10.0);
}

TEST_F(GatewayTest, NoMatch_SellForwardedToExchange) {
    system.handleOrder(
        makeOrder("2001", "XSHE", "000001", "S", 20.0, 50, "SZ001"));

    EXPECT_EQ(clientResponses.size(), 0);
    ASSERT_EQ(exchangeRequests.size(), 1);
    EXPECT_EQ(exchangeRequests[0]["clOrderId"], "2001");
    EXPECT_EQ(exchangeRequests[0]["side"], "S");
}

// ==================== 内部撮合 → 发送撤单请求 ====================

TEST_F(GatewayTest, Match_SendsCancelRequests) {
    // 先挂卖单（无匹配，转发交易所+入内部簿）
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    exchangeRequests.clear();

    // 买单来了，内部撮合成功 → 应向交易所发送撤单请求
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    // 不应直接给客户端发成交回报（需等撤单确认）
    EXPECT_EQ(clientResponses.size(), 0);

    // 应向交易所发送撤单请求（撤S1）
    ASSERT_EQ(exchangeRequests.size(), 1);
    auto &cancelReq = exchangeRequests[0];
    EXPECT_EQ(cancelReq["origClOrderId"], "S1");
    EXPECT_EQ(cancelReq["market"], "XSHG");
    EXPECT_EQ(cancelReq["securityId"], "600030");
    EXPECT_EQ(cancelReq["side"], "S");
    EXPECT_EQ(cancelReq["shareholderId"], "SH002");
}

TEST_F(GatewayTest, Match_MultipleCancelRequests) {
    // 三个卖单挂簿
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 200, "SH002"));
    system.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 10.0, 300, "SH003"));
    system.handleOrder(
        makeOrder("S3", "XSHG", "600030", "S", 10.5, 500, "SH004"));
    exchangeRequests.clear();

    // 大买单来，匹配3个对手方
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.5, 1000, "SH001"));

    EXPECT_EQ(clientResponses.size(), 0);

    // 应发送3个撤单请求
    ASSERT_EQ(exchangeRequests.size(), 3);
    EXPECT_EQ(exchangeRequests[0]["origClOrderId"], "S1");
    EXPECT_EQ(exchangeRequests[1]["origClOrderId"], "S2");
    EXPECT_EQ(exchangeRequests[2]["origClOrderId"], "S3");
}

// ==================== 撤单转发 ====================

TEST_F(GatewayTest, Cancel_ForwardedToExchange) {
    // 撤单应直接转发给交易所
    system.handleCancel(
        makeCancel("C001", "1001", "XSHG", "600030", "SH001", "B"));

    EXPECT_EQ(clientResponses.size(), 0);
    ASSERT_EQ(exchangeRequests.size(), 1);
    EXPECT_EQ(exchangeRequests[0]["clOrderId"], "C001");
    EXPECT_EQ(exchangeRequests[0]["origClOrderId"], "1001");
}

// ==================== 对敲检测 ====================

TEST_F(GatewayTest, CrossTrade_StillRejected) {
    // 同一股东号先买后卖 → 对敲拒绝，不转发交易所
    system.handleOrder(
        makeOrder("1001", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();
    exchangeRequests.clear();

    system.handleOrder(
        makeOrder("2001", "XSHG", "600030", "S", 10.0, 100, "SH001"));

    // 对敲拒绝回报发给客户端
    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["clOrderId"], "2001");
    EXPECT_EQ(clientResponses[0]["rejectCode"], ORDER_CROSS_TRADE_REJECT_CODE);

    // 不应转发交易所
    EXPECT_EQ(exchangeRequests.size(), 0);
}

// ==================== JSON 解析错误 ====================

TEST_F(GatewayTest, InvalidOrderJson_StillRejected) {
    json badOrder = {{"clOrderId", "BAD1"}, {"market", "INVALID"}};
    system.handleOrder(badOrder);

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["rejectCode"],
              ORDER_INVALID_FORMAT_REJECT_CODE);
    EXPECT_EQ(exchangeRequests.size(), 0);
}

TEST_F(GatewayTest, InvalidCancelJson_StillRejected) {
    json badCancel = {{"clOrderId", "C1"}};
    system.handleCancel(badCancel);

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["rejectCode"],
              ORDER_INVALID_FORMAT_REJECT_CODE);
    EXPECT_EQ(exchangeRequests.size(), 0);
}

// ==================== handleResponse: 交易所回报处理 ====================

TEST_F(GatewayTest, ExecReport_ForwardedToClient) {
    // 交易所发来成交回报 → 直接转发给客户端
    json execReport = {{"clOrderId", "1001"},
                       {"execId", "EX001"},
                       {"execQty", 100},
                       {"execPrice", 10.0}};

    system.handleResponse(execReport);

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["clOrderId"], "1001");
    EXPECT_EQ(clientResponses[0]["execId"], "EX001");
    EXPECT_EQ(clientResponses[0]["execQty"], 100);
}

TEST_F(GatewayTest, ConfirmReport_ForwardedToClient) {
    // 交易所发来确认回报（无 execId，无 origClOrderId）→ 直接转发
    json confirmReport = {{"clOrderId", "1001"},
                          {"market", "XSHG"},
                          {"securityId", "600030"},
                          {"status", "confirmed"}};

    system.handleResponse(confirmReport);

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["clOrderId"], "1001");
    EXPECT_EQ(clientResponses[0]["status"], "confirmed");
}

TEST_F(GatewayTest, UserCancelResponse_ForwardedToClient) {
    // 用户主动撤单的确认回报（非内部撮合撤单）→ 直接转发
    json cancelResponse = {
        {"origClOrderId", "1001"}, {"canceledQty", 100}, {"cumQty", 0}};

    system.handleResponse(cancelResponse);

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["origClOrderId"], "1001");
    EXPECT_EQ(clientResponses[0]["canceledQty"], 100);
}

// ==================== PendingMatch: 全部确认 ====================

TEST_F(GatewayTest, PendingMatch_AllConfirmed_ExecReportsSent) {
    // 挂卖单（转发交易所+入内部簿）
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
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

    // 应产生成交回报：被动方 + 主动方
    ASSERT_EQ(clientResponses.size(), 2);

    // 被动方（S1）成交回报
    EXPECT_EQ(clientResponses[0]["clOrderId"], "S1");
    EXPECT_EQ(clientResponses[0]["side"], "S");
    EXPECT_TRUE(clientResponses[0].contains("execId"));
    EXPECT_EQ(clientResponses[0]["execQty"], 100);

    // 主动方（B1）成交回报
    EXPECT_EQ(clientResponses[1]["clOrderId"], "B1");
    EXPECT_EQ(clientResponses[1]["side"], "B");
    EXPECT_EQ(clientResponses[1]["execQty"], 100);

    // 成交回报的 execId 应相同
    EXPECT_EQ(clientResponses[0]["execId"], clientResponses[1]["execId"]);

    // 不应有新的交易所请求（全部确认，无剩余）
    EXPECT_EQ(exchangeRequests.size(), 0);
}

// ==================== PendingMatch: 全部拒绝 ====================

TEST_F(GatewayTest, PendingMatch_AllRejected_RemainingForwarded) {
    // 挂卖单
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    exchangeRequests.clear();

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

TEST_F(GatewayTest, PendingMatch_PartialConfirm_PartialReject) {
    // 两个卖单挂簿
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 200, "SH002"));
    system.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 10.0, 300, "SH003"));
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
    // S1确认 → 成交200（2个回报）
    // S2被拒 → 300股未成交，转发交易所

    // 找成交回报（S1 确认的部分）
    int execReportCount = 0;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            execReportCount++;
            EXPECT_EQ(resp["execQty"], 200);
        }
    }
    EXPECT_EQ(execReportCount, 2); // 被动方 + 主动方

    // 被拒部分（300股）应转发给交易所
    ASSERT_GE(exchangeRequests.size(), 1);
    EXPECT_EQ(exchangeRequests.back()["qty"], 300);
}

// ==================== PendingMatch: 撮合有剩余 + 全部确认 ====================

TEST_F(GatewayTest, PendingMatch_WithRemainingQty_Confirmed) {
    // 卖100挂簿
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    exchangeRequests.clear();

    // 买500来撮合 → 成交100 + 剩余400
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 500, "SH001"));
    clientResponses.clear();
    exchangeRequests.clear();

    // S1 撤单确认
    json confirmS1 = {
        {"origClOrderId", "S1"}, {"canceledQty", 100}, {"cumQty", 0}};
    system.handleResponse(confirmS1);

    // 成交100（2个回报）
    int execReportCount = 0;
    for (const auto &resp : clientResponses) {
        if (resp.contains("execId")) {
            execReportCount++;
            EXPECT_EQ(resp["execQty"], 100);
        }
    }
    EXPECT_EQ(execReportCount, 2);

    // 剩余400转发给交易所
    ASSERT_GE(exchangeRequests.size(), 1);
    EXPECT_EQ(exchangeRequests.back()["qty"], 400);
}

// ==================== PendingMatch: 撮合有剩余 + 部分拒绝 ====================

TEST_F(GatewayTest, PendingMatch_WithRemaining_SomeRejected) {
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

// ==================== handleResponse: 交易所成交 → 内部簿同步
// ====================

TEST_F(GatewayTest, ExecReport_ReducesInternalBook) {
    // 买单入内部簿（无匹配，转发交易所）
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 200, "SH001"));
    exchangeRequests.clear();

    // 交易所成交了100股
    json execReport = {{"clOrderId", "B1"},
                       {"execId", "EX001"},
                       {"execQty", 100},
                       {"execPrice", 10.0}};
    system.handleResponse(execReport);
    clientResponses.clear();

    // 现在内部簿只剩100股
    // 挂卖单来验证：内部只能撮合100
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 200, "SH002"));

    // 内部撮合应只成交100（内部簿只有100），发1个撤单请求
    ASSERT_EQ(exchangeRequests.size(), 1);
    EXPECT_EQ(exchangeRequests[0]["origClOrderId"], "B1");
}

TEST_F(GatewayTest, ExecReport_FullyFilled_RemovedFromBook) {
    // 买单入内部簿
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    exchangeRequests.clear();

    // 交易所完全成交了
    json execReport = {{"clOrderId", "B1"},
                       {"execId", "EX001"},
                       {"execQty", 100},
                       {"execPrice", 10.0}};
    system.handleResponse(execReport);
    clientResponses.clear();
    exchangeRequests.clear();

    // 卖单来了，不应匹配已成交的B1
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));

    // 无匹配 → 转发交易所（非撤单请求）
    ASSERT_EQ(exchangeRequests.size(), 1);
    EXPECT_EQ(exchangeRequests[0]["clOrderId"], "S1"); // 是新订单转发，不是撤单
    EXPECT_FALSE(exchangeRequests[0].contains("origClOrderId"));
}

// ==================== 前置模式下内部撮合仍检测对敲 ====================

TEST_F(GatewayTest, InternalMatch_CrossTradeStillBlocked) {
    // SH001 挂卖单（转发交易所+入内部簿）
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH001"));
    clientResponses.clear();
    exchangeRequests.clear();

    // SH001 再挂买单 → 对敲拒绝
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));

    ASSERT_EQ(clientResponses.size(), 1);
    EXPECT_EQ(clientResponses[0]["rejectCode"], ORDER_CROSS_TRADE_REJECT_CODE);
    EXPECT_EQ(exchangeRequests.size(), 0);
}

// ==================== 成交价是 maker price ====================

TEST_F(GatewayTest, PendingMatch_MakerPrice) {
    // 卖方挂 9.5（转发交易所）
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 9.5, 100, "SH002"));
    exchangeRequests.clear();

    // 买方出 10.0 → 成交价应为 9.5（maker price）
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();
    exchangeRequests.clear();

    // 撤单确认
    json confirmS1 = {
        {"origClOrderId", "S1"}, {"canceledQty", 100}, {"cumQty", 0}};
    system.handleResponse(confirmS1);

    // 成交价应为 maker price 9.5
    ASSERT_EQ(clientResponses.size(), 2);
    EXPECT_DOUBLE_EQ(clientResponses[0]["execPrice"].get<double>(), 9.5);
    EXPECT_DOUBLE_EQ(clientResponses[1]["execPrice"].get<double>(), 9.5);
}

// ==================== 多个 PendingMatch 互不干扰 ====================

TEST_F(GatewayTest, MultiplePendingMatches_Independent) {
    // 挂两个卖单（不同股票）
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    system.handleOrder(
        makeOrder("S2", "XSHE", "000001", "S", 20.0, 200, "SH003"));
    exchangeRequests.clear();

    // 两个买单分别匹配
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    system.handleOrder(
        makeOrder("B2", "XSHE", "000001", "B", 20.0, 200, "SH004"));
    clientResponses.clear();
    exchangeRequests.clear();

    // 先回复 S2 的撤单确认
    json confirmS2 = {
        {"origClOrderId", "S2"}, {"canceledQty", 200}, {"cumQty", 0}};
    system.handleResponse(confirmS2);

    // 应只处理 B2 的成交，B1 还在等
    ASSERT_EQ(clientResponses.size(), 2);
    // B2 的成交回报
    bool foundB2 = false;
    for (const auto &resp : clientResponses) {
        if (resp["clOrderId"] == "B2") {
            foundB2 = true;
            EXPECT_EQ(resp["execQty"], 200);
        }
    }
    EXPECT_TRUE(foundB2);

    clientResponses.clear();

    // 再回复 S1 的撤单确认
    json confirmS1 = {
        {"origClOrderId", "S1"}, {"canceledQty", 100}, {"cumQty", 0}};
    system.handleResponse(confirmS1);

    // 应处理 B1 的成交
    ASSERT_EQ(clientResponses.size(), 2);
    bool foundB1 = false;
    for (const auto &resp : clientResponses) {
        if (resp["clOrderId"] == "B1") {
            foundB1 = true;
            EXPECT_EQ(resp["execQty"], 100);
        }
    }
    EXPECT_TRUE(foundB1);
}

// ==================== 价格优先在前置模式下仍有效 ====================

TEST_F(GatewayTest, PricePriority_InGatewayMode) {
    // 挂两个卖单：贵→便宜
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 11.0, 100, "SH002"));
    system.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 10.0, 100, "SH003"));
    exchangeRequests.clear();

    // 买单 11.0，应优先匹配便宜的 S2
    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 11.0, 100, "SH001"));

    // 应只撤 S2（价格优先匹配）
    ASSERT_EQ(exchangeRequests.size(), 1);
    EXPECT_EQ(exchangeRequests[0]["origClOrderId"], "S2");
}

// ==================== 连续内部撮合 ====================

TEST_F(GatewayTest, SequentialInternalMatches) {
    // 第一轮：S1 挂簿 → B1 匹配 → 撤单确认 → 成交
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    exchangeRequests.clear();

    system.handleOrder(
        makeOrder("B1", "XSHG", "600030", "B", 10.0, 100, "SH001"));
    clientResponses.clear();
    exchangeRequests.clear();

    json confirmS1 = {
        {"origClOrderId", "S1"}, {"canceledQty", 100}, {"cumQty", 0}};
    system.handleResponse(confirmS1);

    ASSERT_EQ(clientResponses.size(), 2);
    clientResponses.clear();
    exchangeRequests.clear();

    // 第二轮：S2 挂簿 → B2 匹配 → 撤单确认 → 成交
    system.handleOrder(
        makeOrder("S2", "XSHG", "600030", "S", 11.0, 200, "SH003"));
    exchangeRequests.clear();

    system.handleOrder(
        makeOrder("B2", "XSHG", "600030", "B", 11.0, 200, "SH004"));
    clientResponses.clear();
    exchangeRequests.clear();

    json confirmS2 = {
        {"origClOrderId", "S2"}, {"canceledQty", 200}, {"cumQty", 0}};
    system.handleResponse(confirmS2);

    ASSERT_EQ(clientResponses.size(), 2);
    EXPECT_EQ(clientResponses[0]["execQty"], 200);
}

// ==================== 不同市场/股票不匹配 ====================

TEST_F(GatewayTest, DifferentSecurity_NoInternalMatch) {
    system.handleOrder(
        makeOrder("S1", "XSHG", "600030", "S", 10.0, 100, "SH002"));
    exchangeRequests.clear();

    system.handleOrder(
        makeOrder("B1", "XSHG", "601318", "B", 10.0, 100, "SH001"));

    // 不同股票，无匹配 → 转发交易所（非撤单请求）
    ASSERT_EQ(exchangeRequests.size(), 1);
    EXPECT_EQ(exchangeRequests[0]["clOrderId"], "B1");
    EXPECT_FALSE(exchangeRequests[0].contains("origClOrderId"));
}
