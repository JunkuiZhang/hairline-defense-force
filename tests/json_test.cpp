#include "types.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace hdf;
using json = nlohmann::json;

// ==================== 订单反序列化 ====================

TEST(OrderFromJson, ValidOrder) {
    json j = {{"clOrderId", "1001"},     {"market", "XSHG"},
              {"securityId", "600030"},  {"side", "B"},
              {"price", 10.5},           {"qty", 1000},
              {"shareholderId", "SH001"}};

    Order order = j.get<Order>();

    EXPECT_EQ(order.clOrderId, "1001");
    EXPECT_EQ(order.market, Market::XSHG);
    EXPECT_EQ(order.securityId, "600030");
    EXPECT_EQ(order.side, Side::BUY);
    EXPECT_DOUBLE_EQ(order.price, 10.5);
    EXPECT_EQ(order.qty, 1000);
    EXPECT_EQ(order.shareholderId, "SH001");
}

TEST(OrderFromJson, SellSideOddLot) {
    // 卖方可以不为100的倍数
    json j = {{"clOrderId", "1002"},     {"market", "XSHE"},
              {"securityId", "000001"},  {"side", "S"},
              {"price", 20.0},           {"qty", 50},
              {"shareholderId", "SZ001"}};

    Order order = j.get<Order>();
    EXPECT_EQ(order.side, Side::SELL);
    EXPECT_EQ(order.qty, 50);
}

TEST(OrderFromJson, SellSide) {
    json j = {{"clOrderId", "1002"},     {"market", "XSHE"},
              {"securityId", "000001"},  {"side", "S"},
              {"price", 20.0},           {"qty", 500},
              {"shareholderId", "SZ001"}};

    Order order = j.get<Order>();

    EXPECT_EQ(order.side, Side::SELL);
    EXPECT_EQ(order.market, Market::XSHE);
}

TEST(OrderFromJson, BJSEMarket) {
    json j = {{"clOrderId", "1003"},
              {"market", "BJSE"},
              {"securityId", "430047"},
              {"side", "B"},
              {"price", 5.0},
              {"qty", 100},
              {"shareholderId", "BJ001"}};

    Order order = j.get<Order>();
    EXPECT_EQ(order.market, Market::BJSE);
}

TEST(OrderFromJson, MissingClOrderId) {
    json j = {{"market", "XSHG"}, {"securityId", "600030"},
              {"side", "B"},      {"price", 10.5},
              {"qty", 1000},      {"shareholderId", "SH001"}};

    EXPECT_THROW(j.get<Order>(), nlohmann::json::out_of_range);
}

TEST(OrderFromJson, MissingMarket) {
    json j = {{"clOrderId", "1001"}, {"securityId", "600030"},
              {"side", "B"},         {"price", 10.5},
              {"qty", 1000},         {"shareholderId", "SH001"}};

    EXPECT_THROW(j.get<Order>(), nlohmann::json::out_of_range);
}

TEST(OrderFromJson, MissingSide) {
    json j = {
        {"clOrderId", "1001"}, {"market", "XSHG"}, {"securityId", "600030"},
        {"price", 10.5},       {"qty", 1000},      {"shareholderId", "SH001"}};

    EXPECT_THROW(j.get<Order>(), nlohmann::json::out_of_range);
}

TEST(OrderFromJson, MissingPrice) {
    json j = {
        {"clOrderId", "1001"}, {"market", "XSHG"}, {"securityId", "600030"},
        {"side", "B"},         {"qty", 1000},      {"shareholderId", "SH001"}};

    EXPECT_THROW(j.get<Order>(), nlohmann::json::out_of_range);
}

TEST(OrderFromJson, InvalidMarket) {
    json j = {{"clOrderId", "1001"},     {"market", "NYSE"},
              {"securityId", "600030"},  {"side", "B"},
              {"price", 10.5},           {"qty", 1000},
              {"shareholderId", "SH001"}};

    EXPECT_THROW(j.get<Order>(), std::invalid_argument);
}

TEST(OrderFromJson, InvalidSide) {
    json j = {{"clOrderId", "1001"},     {"market", "XSHG"},
              {"securityId", "600030"},  {"side", "X"},
              {"price", 10.5},           {"qty", 1000},
              {"shareholderId", "SH001"}};

    EXPECT_THROW(j.get<Order>(), std::invalid_argument);
}

TEST(OrderFromJson, WrongTypeForPrice) {
    json j = {{"clOrderId", "1001"},     {"market", "XSHG"},
              {"securityId", "600030"},  {"side", "B"},
              {"price", "not_a_number"}, {"qty", 1000},
              {"shareholderId", "SH001"}};

    EXPECT_THROW(j.get<Order>(), nlohmann::json::type_error);
}

TEST(OrderFromJson, WrongTypeForQty) {
    json j = {{"clOrderId", "1001"},     {"market", "XSHG"},
              {"securityId", "600030"},  {"side", "B"},
              {"price", 10.5},           {"qty", "not_a_number"},
              {"shareholderId", "SH001"}};

    EXPECT_THROW(j.get<Order>(), nlohmann::json::type_error);
}

TEST(OrderFromJson, NegativePrice) {
    json j = {{"clOrderId", "1001"},     {"market", "XSHG"},
              {"securityId", "600030"},  {"side", "B"},
              {"price", -1.0},           {"qty", 100},
              {"shareholderId", "SH001"}};

    EXPECT_THROW(j.get<Order>(), std::invalid_argument);
}

TEST(OrderFromJson, ZeroPrice) {
    json j = {{"clOrderId", "1001"},
              {"market", "XSHG"},
              {"securityId", "600030"},
              {"side", "B"},
              {"price", 0.0},
              {"qty", 100},
              {"shareholderId", "SH001"}};

    EXPECT_THROW(j.get<Order>(), std::invalid_argument);
}

TEST(OrderFromJson, ZeroQty) {
    json j = {{"clOrderId", "1001"},     {"market", "XSHG"},
              {"securityId", "600030"},  {"side", "B"},
              {"price", 10.0},           {"qty", 0},
              {"shareholderId", "SH001"}};

    EXPECT_THROW(j.get<Order>(), std::invalid_argument);
}

TEST(OrderFromJson, BuyQtyNotMultipleOf100) {
    json j = {{"clOrderId", "1001"},     {"market", "XSHG"},
              {"securityId", "600030"},  {"side", "B"},
              {"price", 10.0},           {"qty", 150},
              {"shareholderId", "SH001"}};

    EXPECT_THROW(j.get<Order>(), std::invalid_argument);
}

TEST(OrderFromJson, BuyQtyMultipleOf100) {
    json j = {{"clOrderId", "1001"},     {"market", "XSHG"},
              {"securityId", "600030"},  {"side", "B"},
              {"price", 10.0},           {"qty", 300},
              {"shareholderId", "SH001"}};

    Order order = j.get<Order>();
    EXPECT_EQ(order.qty, 300);
}

// ==================== 撤销订单的反序列化 ====================

TEST(CancelOrderFromJson, ValidCancelOrder) {
    json j = {{"clOrderId", "C001"},      {"origClOrderId", "1001"},
              {"market", "XSHG"},         {"securityId", "600030"},
              {"shareholderId", "SH001"}, {"side", "B"}};

    CancelOrder order = j.get<CancelOrder>();

    EXPECT_EQ(order.clOrderId, "C001");
    EXPECT_EQ(order.origClOrderId, "1001");
    EXPECT_EQ(order.market, Market::XSHG);
    EXPECT_EQ(order.securityId, "600030");
    EXPECT_EQ(order.shareholderId, "SH001");
    EXPECT_EQ(order.side, Side::BUY);
}

TEST(CancelOrderFromJson, MissingOrigClOrderId) {
    json j = {{"clOrderId", "C001"},
              {"market", "XSHG"},
              {"securityId", "600030"},
              {"shareholderId", "SH001"},
              {"side", "B"}};

    EXPECT_THROW(j.get<CancelOrder>(), nlohmann::json::out_of_range);
}

TEST(CancelOrderFromJson, InvalidMarket) {
    json j = {{"clOrderId", "C001"},      {"origClOrderId", "1001"},
              {"market", "INVALID"},      {"securityId", "600030"},
              {"shareholderId", "SH001"}, {"side", "B"}};

    EXPECT_THROW(j.get<CancelOrder>(), std::invalid_argument);
}

TEST(CancelOrderFromJson, InvalidSide) {
    json j = {{"clOrderId", "C001"},      {"origClOrderId", "1001"},
              {"market", "XSHG"},         {"securityId", "600030"},
              {"shareholderId", "SH001"}, {"side", "INVALID"}};

    EXPECT_THROW(j.get<CancelOrder>(), std::invalid_argument);
}

TEST(CancelOrderFromJson, EmptyJson) {
    json j = json::object();
    EXPECT_THROW(j.get<CancelOrder>(), nlohmann::json::out_of_range);
}

// ==================== 枚举转换 ====================

TEST(EnumConversion, SideToString) {
    EXPECT_EQ(to_string(Side::BUY), "B");
    EXPECT_EQ(to_string(Side::SELL), "S");
}

TEST(EnumConversion, SideFromString) {
    EXPECT_EQ(side_from_string("B"), Side::BUY);
    EXPECT_EQ(side_from_string("S"), Side::SELL);
    EXPECT_THROW(side_from_string("X"), std::invalid_argument);
    EXPECT_THROW(side_from_string(""), std::invalid_argument);
}

TEST(EnumConversion, MarketToString) {
    EXPECT_EQ(to_string(Market::XSHG), "XSHG");
    EXPECT_EQ(to_string(Market::XSHE), "XSHE");
    EXPECT_EQ(to_string(Market::BJSE), "BJSE");
}

TEST(EnumConversion, MarketFromString) {
    EXPECT_EQ(market_from_string("XSHG"), Market::XSHG);
    EXPECT_EQ(market_from_string("XSHE"), Market::XSHE);
    EXPECT_EQ(market_from_string("BJSE"), Market::BJSE);
    EXPECT_THROW(market_from_string("NYSE"), std::invalid_argument);
    EXPECT_THROW(market_from_string(""), std::invalid_argument);
}
