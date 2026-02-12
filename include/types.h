#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace hdf {

enum class Side { BUY, SELL, UNKNOWN };

inline std::string to_string(Side s) {
    switch (s) {
    case Side::BUY:
        return "B";
    case Side::SELL:
        return "S";
    case Side::UNKNOWN:
    default:
        std::unreachable(); // 执行到此处为UB
    }
}

inline Side side_from_string(const std::string &s) {
    if (s == "B")
        return Side::BUY;
    if (s == "S")
        return Side::SELL;
    throw std::invalid_argument("Invalid side: " + s);
}

enum class Market { XSHG, XSHE, BJSE, UNKNOWN };

inline std::string to_string(Market m) {
    switch (m) {
    case Market::XSHG:
        return "XSHG";
    case Market::XSHE:
        return "XSHE";
    case Market::BJSE:
        return "BJSE";
    case Market::UNKNOWN:
    default:
        std::unreachable(); // 执行到此处为UB
    }
}

inline Market market_from_string(const std::string &s) {
    if (s == "XSHG")
        return Market::XSHG;
    if (s == "XSHE")
        return Market::XSHE;
    if (s == "BJSE")
        return Market::BJSE;
    throw std::invalid_argument("Invalid market: " + s);
}

// 3.1 交易订单
struct Order {
    std::string clOrderId;
    Market market;
    std::string securityId;
    Side side;
    double price;
    uint32_t qty;
    std::string shareholderId;
};

inline void from_json(const nlohmann::json &j, Order &o) {
    j.at("clOrderId").get_to(o.clOrderId);
    o.market = market_from_string(j.at("market").get<std::string>());
    j.at("securityId").get_to(o.securityId);
    o.side = side_from_string(j.at("side").get<std::string>());
    j.at("price").get_to(o.price);
    j.at("qty").get_to(o.qty);
    j.at("shareholderId").get_to(o.shareholderId);

    if (o.price <= 0) {
        throw std::invalid_argument("price must be positive, got: " +
                                    std::to_string(o.price));
    }
    if (o.qty == 0) {
        throw std::invalid_argument("qty must be positive");
    }
    if (o.side == Side::BUY && o.qty % 100 != 0) {
        throw std::invalid_argument("buy qty must be a multiple of 100, got: " +
                                    std::to_string(o.qty));
    }
}

// 3.2 交易撤单
struct CancelOrder {
    std::string clOrderId;
    std::string origClOrderId;
    Market market;
    std::string securityId;
    std::string shareholderId;
    Side side;
};

inline void from_json(const nlohmann::json &j, CancelOrder &o) {
    j.at("clOrderId").get_to(o.clOrderId);
    j.at("origClOrderId").get_to(o.origClOrderId);
    o.market = market_from_string(j.at("market").get<std::string>());
    j.at("securityId").get_to(o.securityId);
    j.at("shareholderId").get_to(o.shareholderId);
    o.side = side_from_string(j.at("side").get<std::string>());
}

// 3.3 行情信息
struct MarketData {
    Market market;
    std::string securityId;
    double bidPrice;
    double askPrice;
};

// 3.4 - 3.8 输出结构体（可以统一也可以分开）
struct OrderResponse {
    std::string clOrderId;
    Market market;
    std::string securityId;
    Side side;
    uint32_t qty;
    double price;
    std::string shareholderId;

    // 拒绝信息
    int32_t rejectCode = 0;
    std::string rejectText;

    // 成交信息
    std::string execId;
    uint32_t execQty = 0;
    double execPrice = 0.0;

    // 类型
    enum Type { CONFIRM, REJECT, EXECUTION } type;
};

struct CancelResponse {
    std::string clOrderId;
    std::string origClOrderId;
    Market market;
    std::string securityId;
    std::string shareholderId;
    Side side;

    // 确认信息
    uint32_t qty = 0;
    double price = 0.0;
    uint32_t cumQty = 0;
    uint32_t canceledQty = 0;

    // 拒绝信息
    int32_t rejectCode = 0;
    std::string rejectText;

    enum Type { CONFIRM, REJECT } type;
};

} // namespace hdf
