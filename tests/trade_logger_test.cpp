#include "trade_logger.h"
#include "types.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

using namespace hdf;
using json = nlohmann::json;

// ============================================================
// 测试夹具：每个测试用例使用独立的临时文件
// ============================================================
class TradeLoggerTest : public ::testing::Test {
  protected:
    TradeLogger logger;
    std::string testFile;

    void SetUp() override {
        // 用测试名生成独立文件路径，避免冲突
        auto info = ::testing::UnitTest::GetInstance()->current_test_info();
        testFile = std::string("data/test_logs/") + info->name() + ".jsonl";
        // 确保旧文件不存在
        std::filesystem::remove(testFile);
    }

    void TearDown() override {
        if (logger.isOpen())
            logger.close();
        std::filesystem::remove(testFile);
    }

    /**
     * @brief 读取 JSONL 文件中的所有记录
     */
    std::vector<json> readRecords() {
        std::vector<json> records;
        std::ifstream f(testFile);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty())
                records.push_back(json::parse(line));
        }
        return records;
    }

    /**
     * @brief 辅助函数：创建测试用订单
     */
    Order createOrder(const std::string &id = "ORD001",
                      const std::string &securityId = "600030",
                      Side side = Side::BUY, double price = 25.50,
                      uint32_t qty = 1000,
                      const std::string &shareholderId = "SH001") {
        Order order;
        order.clOrderId = id;
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
// 文件管理测试
// ============================================================

/**
 * @brief 测试：正常打开和关闭日志文件
 */
TEST_F(TradeLoggerTest, OpenAndClose) {
    EXPECT_FALSE(logger.isOpen());

    EXPECT_TRUE(logger.open(testFile));
    EXPECT_TRUE(logger.isOpen());

    logger.close();
    EXPECT_FALSE(logger.isOpen());
}

/**
 * @brief 测试：自动创建父目录
 */
TEST_F(TradeLoggerTest, CreatesParentDirectories) {
    std::string deepPath = "data/test_logs/deep/nested/dir/test.jsonl";
    std::filesystem::remove_all("data/test_logs/deep");

    EXPECT_TRUE(logger.open(deepPath));
    EXPECT_TRUE(std::filesystem::exists(deepPath));

    logger.close();
    std::filesystem::remove_all("data/test_logs/deep");
}

/**
 * @brief 测试：重复 open 会先 close 再重新打开
 */
TEST_F(TradeLoggerTest, ReopenClosesFirst) {
    EXPECT_TRUE(logger.open(testFile));
    logger.logOrderConfirm("ORD001");
    // 再次 open 另一个文件
    std::string file2 = "data/test_logs/reopen_test2.jsonl";
    std::filesystem::remove(file2);

    EXPECT_TRUE(logger.open(file2));
    EXPECT_TRUE(logger.isOpen());

    logger.close();
    std::filesystem::remove(file2);
}

/**
 * @brief 测试：重复 close 不会崩溃
 */
TEST_F(TradeLoggerTest, DoubleCloseIsSafe) {
    EXPECT_TRUE(logger.open(testFile));
    logger.close();
    EXPECT_NO_FATAL_FAILURE(logger.close());
    EXPECT_FALSE(logger.isOpen());
}

/**
 * @brief 测试：未 open 时调用 log 方法不会崩溃
 */
TEST_F(TradeLoggerTest, LogWithoutOpenIsSafe) {
    EXPECT_FALSE(logger.isOpen());
    EXPECT_NO_FATAL_FAILURE(logger.logOrderNew(createOrder()));
    EXPECT_NO_FATAL_FAILURE(logger.logOrderConfirm("ORD001"));
    EXPECT_NO_FATAL_FAILURE(logger.logOrderReject("ORD001", 1, "some reason"));
    EXPECT_NO_FATAL_FAILURE(logger.logExecution("EXEC001", "ORD001", "600030",
                                                Side::BUY, 100, 25.5, true));
    EXPECT_NO_FATAL_FAILURE(logger.logCancelConfirm("ORD001", 500, 200));
    EXPECT_NO_FATAL_FAILURE(logger.logCancelReject("ORD001", 2, "not found"));
    EXPECT_NO_FATAL_FAILURE(
        logger.logMarketData("600030", Market::XSHG, 25.0, 25.5));
}

// ============================================================
// 事件记录测试
// ============================================================

/**
 * @brief 测试：logOrderNew 正确记录所有字段
 */
TEST_F(TradeLoggerTest, LogOrderNew) {
    ASSERT_TRUE(logger.open(testFile));

    Order order =
        createOrder("ORD100", "600519", Side::SELL, 1800.0, 200, "SH005");
    logger.logOrderNew(order);
    logger.close();

    auto records = readRecords();
    ASSERT_EQ(records.size(), 1);

    auto &r = records[0];
    EXPECT_EQ(r["event"], "ORDER_NEW");
    EXPECT_EQ(r["clOrderId"], "ORD100");
    EXPECT_EQ(r["market"], "XSHG");
    EXPECT_EQ(r["securityId"], "600519");
    EXPECT_EQ(r["side"], "S");
    EXPECT_DOUBLE_EQ(r["price"].get<double>(), 1800.0);
    EXPECT_EQ(r["qty"], 200);
    EXPECT_EQ(r["shareholderId"], "SH005");
    EXPECT_TRUE(r.contains("timestamp"));
}

/**
 * @brief 测试：logOrderConfirm 正确记录
 */
TEST_F(TradeLoggerTest, LogOrderConfirm) {
    ASSERT_TRUE(logger.open(testFile));
    logger.logOrderConfirm("ORD200");
    logger.close();

    auto records = readRecords();
    ASSERT_EQ(records.size(), 1);

    EXPECT_EQ(records[0]["event"], "ORDER_CONFIRM");
    EXPECT_EQ(records[0]["clOrderId"], "ORD200");
    EXPECT_TRUE(records[0].contains("timestamp"));
}

/**
 * @brief 测试：logOrderReject 正确记录拒绝码和原因
 */
TEST_F(TradeLoggerTest, LogOrderReject) {
    ASSERT_TRUE(logger.open(testFile));
    logger.logOrderReject("ORD300", 1001, "Cross trade detected");
    logger.close();

    auto records = readRecords();
    ASSERT_EQ(records.size(), 1);

    auto &r = records[0];
    EXPECT_EQ(r["event"], "ORDER_REJECT");
    EXPECT_EQ(r["clOrderId"], "ORD300");
    EXPECT_EQ(r["rejectCode"], 1001);
    EXPECT_EQ(r["rejectText"], "Cross trade detected");
}

/**
 * @brief 测试：logExecution 正确记录成交细节
 */
TEST_F(TradeLoggerTest, LogExecution) {
    ASSERT_TRUE(logger.open(testFile));
    logger.logExecution("EXEC001", "ORD400", "000001", Side::BUY, 500, 15.30,
                        true);
    logger.close();

    auto records = readRecords();
    ASSERT_EQ(records.size(), 1);

    auto &r = records[0];
    EXPECT_EQ(r["event"], "EXECUTION");
    EXPECT_EQ(r["execId"], "EXEC001");
    EXPECT_EQ(r["clOrderId"], "ORD400");
    EXPECT_EQ(r["securityId"], "000001");
    EXPECT_EQ(r["side"], "B");
    EXPECT_EQ(r["execQty"], 500);
    EXPECT_DOUBLE_EQ(r["execPrice"].get<double>(), 15.30);
    EXPECT_TRUE(r["isMaker"].get<bool>());
}

/**
 * @brief 测试：logExecution isMaker=false
 */
TEST_F(TradeLoggerTest, LogExecutionTaker) {
    ASSERT_TRUE(logger.open(testFile));
    logger.logExecution("EXEC002", "ORD401", "600030", Side::SELL, 300, 28.0,
                        false);
    logger.close();

    auto records = readRecords();
    ASSERT_EQ(records.size(), 1);

    EXPECT_EQ(records[0]["side"], "S");
    EXPECT_FALSE(records[0]["isMaker"].get<bool>());
}

/**
 * @brief 测试：logCancelConfirm 正确记录撤单确认
 */
TEST_F(TradeLoggerTest, LogCancelConfirm) {
    ASSERT_TRUE(logger.open(testFile));
    logger.logCancelConfirm("ORD500", 800, 200);
    logger.close();

    auto records = readRecords();
    ASSERT_EQ(records.size(), 1);

    auto &r = records[0];
    EXPECT_EQ(r["event"], "CANCEL_CONFIRM");
    EXPECT_EQ(r["origClOrderId"], "ORD500");
    EXPECT_EQ(r["canceledQty"], 800);
    EXPECT_EQ(r["cumQty"], 200);
}

/**
 * @brief 测试：logCancelReject 正确记录撤单拒绝
 */
TEST_F(TradeLoggerTest, LogCancelReject) {
    ASSERT_TRUE(logger.open(testFile));
    logger.logCancelReject("ORD600", 2001, "Order fully filled");
    logger.close();

    auto records = readRecords();
    ASSERT_EQ(records.size(), 1);

    auto &r = records[0];
    EXPECT_EQ(r["event"], "CANCEL_REJECT");
    EXPECT_EQ(r["origClOrderId"], "ORD600");
    EXPECT_EQ(r["rejectCode"], 2001);
    EXPECT_EQ(r["rejectText"], "Order fully filled");
}

/**
 * @brief 测试：logMarketData 正确记录行情快照
 */
TEST_F(TradeLoggerTest, LogMarketData) {
    ASSERT_TRUE(logger.open(testFile));
    logger.logMarketData("600519", Market::XSHG, 1795.0, 1805.0);
    logger.close();

    auto records = readRecords();
    ASSERT_EQ(records.size(), 1);

    auto &r = records[0];
    EXPECT_EQ(r["event"], "MARKET_DATA");
    EXPECT_EQ(r["securityId"], "600519");
    EXPECT_EQ(r["market"], "XSHG");
    EXPECT_DOUBLE_EQ(r["bidPrice"].get<double>(), 1795.0);
    EXPECT_DOUBLE_EQ(r["askPrice"].get<double>(), 1805.0);
}

// ============================================================
// 时间戳测试
// ============================================================

/**
 * @brief 测试：时间戳为合理的毫秒级 Unix 时间
 */
TEST_F(TradeLoggerTest, TimestampIsReasonable) {
    ASSERT_TRUE(logger.open(testFile));

    auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    logger.logOrderConfirm("ORD_TS");
    logger.close();
    auto after = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();

    auto records = readRecords();
    ASSERT_EQ(records.size(), 1);

    int64_t ts = records[0]["timestamp"].get<int64_t>();
    EXPECT_GE(ts, before);
    EXPECT_LE(ts, after);
}

/**
 * @brief 测试：多条记录的时间戳单调递增
 */
TEST_F(TradeLoggerTest, TimestampsAreNonDecreasing) {
    ASSERT_TRUE(logger.open(testFile));

    for (int i = 0; i < 10; ++i) {
        logger.logOrderConfirm("ORD_" + std::to_string(i));
    }
    logger.close();

    auto records = readRecords();
    ASSERT_EQ(records.size(), 10);

    for (size_t i = 1; i < records.size(); ++i) {
        EXPECT_GE(records[i]["timestamp"].get<int64_t>(),
                  records[i - 1]["timestamp"].get<int64_t>());
    }
}

// ============================================================
// 批量写入 & 异步队列测试
// ============================================================

/**
 * @brief 测试：批量写入多种事件类型，全部正确持久化
 */
TEST_F(TradeLoggerTest, MultipleMixedEvents) {
    ASSERT_TRUE(logger.open(testFile));

    logger.logOrderNew(createOrder("M001", "600030", Side::BUY, 25.0, 1000));
    logger.logOrderConfirm("M001");
    logger.logExecution("EXEC_M1", "M001", "600030", Side::BUY, 500, 25.0,
                        false);
    logger.logOrderNew(createOrder("M002", "600030", Side::SELL, 25.0, 500));
    logger.logOrderConfirm("M002");
    logger.logExecution("EXEC_M2", "M002", "600030", Side::SELL, 500, 25.0,
                        true);
    logger.logCancelConfirm("M001", 500, 500);
    logger.logOrderReject("M003", 99, "invalid price");
    logger.logCancelReject("M004", 88, "not found");
    logger.logMarketData("600030", Market::XSHG, 24.5, 25.5);

    logger.close();

    auto records = readRecords();
    ASSERT_EQ(records.size(), 10);

    // 验证事件顺序
    EXPECT_EQ(records[0]["event"], "ORDER_NEW");
    EXPECT_EQ(records[1]["event"], "ORDER_CONFIRM");
    EXPECT_EQ(records[2]["event"], "EXECUTION");
    EXPECT_EQ(records[3]["event"], "ORDER_NEW");
    EXPECT_EQ(records[4]["event"], "ORDER_CONFIRM");
    EXPECT_EQ(records[5]["event"], "EXECUTION");
    EXPECT_EQ(records[6]["event"], "CANCEL_CONFIRM");
    EXPECT_EQ(records[7]["event"], "ORDER_REJECT");
    EXPECT_EQ(records[8]["event"], "CANCEL_REJECT");
    EXPECT_EQ(records[9]["event"], "MARKET_DATA");
}

/**
 * @brief 测试：大批量写入不丢数据
 */
TEST_F(TradeLoggerTest, HighVolumeNoDataloss) {
    ASSERT_TRUE(logger.open(testFile));

    const int N = 1000;
    for (int i = 0; i < N; ++i) {
        logger.logOrderConfirm("BULK_" + std::to_string(i));
    }
    logger.close();

    auto records = readRecords();
    ASSERT_EQ(static_cast<int>(records.size()), N);

    // 验证每条记录是完整的 JSON
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(records[i]["event"], "ORDER_CONFIRM");
        EXPECT_EQ(records[i]["clOrderId"], "BULK_" + std::to_string(i));
    }
}

/**
 * @brief 测试：追加模式 — 两次打开同一文件不会覆盖旧数据
 */
TEST_F(TradeLoggerTest, AppendMode) {
    // 第一次写入
    {
        TradeLogger logger1;
        ASSERT_TRUE(logger1.open(testFile));
        logger1.logOrderConfirm("FIRST_001");
        logger1.logOrderConfirm("FIRST_002");
        logger1.close();
    }

    // 第二次写入同一文件
    {
        TradeLogger logger2;
        ASSERT_TRUE(logger2.open(testFile));
        logger2.logOrderConfirm("SECOND_001");
        logger2.close();
    }

    auto records = readRecords();
    ASSERT_EQ(records.size(), 3);
    EXPECT_EQ(records[0]["clOrderId"], "FIRST_001");
    EXPECT_EQ(records[1]["clOrderId"], "FIRST_002");
    EXPECT_EQ(records[2]["clOrderId"], "SECOND_001");
}

/**
 * @brief 测试：多线程并发写入不丢数据
 */
TEST_F(TradeLoggerTest, ConcurrentWrites) {
    ASSERT_TRUE(logger.open(testFile));

    const int NUM_THREADS = 4;
    const int PER_THREAD = 250;

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                std::string id =
                    "T" + std::to_string(t) + "_" + std::to_string(i);
                logger.logOrderConfirm(id);
            }
        });
    }
    for (auto &th : threads) {
        th.join();
    }

    logger.close();

    auto records = readRecords();
    ASSERT_EQ(static_cast<int>(records.size()), NUM_THREADS * PER_THREAD);

    // 验证所有记录都是 ORDER_CONFIRM
    for (auto &r : records) {
        EXPECT_EQ(r["event"], "ORDER_CONFIRM");
        EXPECT_TRUE(r.contains("clOrderId"));
        EXPECT_TRUE(r.contains("timestamp"));
    }
}

// ============================================================
// 每条记录的 JSONL 格式合法性
// ============================================================

/**
 * @brief 测试：输出文件每行都是合法 JSON
 */
TEST_F(TradeLoggerTest, EachLineIsValidJson) {
    ASSERT_TRUE(logger.open(testFile));

    logger.logOrderNew(createOrder());
    logger.logOrderConfirm("V001");
    logger.logOrderReject("V002", 1, "bad");
    logger.logExecution("E001", "V003", "600030", Side::BUY, 100, 10.0, true);
    logger.logCancelConfirm("V004", 100, 0);
    logger.logCancelReject("V005", 2, "no");
    logger.logMarketData("600030", Market::XSHG, 9.0, 11.0);
    logger.close();

    std::ifstream f(testFile);
    std::string line;
    int lineNum = 0;
    while (std::getline(f, line)) {
        if (line.empty())
            continue;
        lineNum++;
        EXPECT_NO_THROW({ json parsed = json::parse(line); })
            << "Line " << lineNum << " is not valid JSON: " << line;
    }
    EXPECT_EQ(lineNum, 7);
}
