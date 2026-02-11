#pragma once

#include <cstdint>
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

// 3.2 交易撤单
struct CancelOrder {
    std::string clOrderId;
    std::string origClOrderId;
    Market market;
    std::string securityId;
    std::string shareholderId;
    Side side;
};

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
