#pragma once

#include "fixed_string.h"
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace hdf {

// ── 定长字符串类型别名 ────────────────────────────────────────
using OrderId       = FixedString<24>;  // clOrderId, origClOrderId
using SecurityId    = FixedString<8>;   // securityId (e.g. "600030")
using ShareholderId = FixedString<16>;  // shareholderId (e.g. "SH001")
using ExecId        = FixedString<24>;  // execId (e.g. "EXEC0000000000000001")
using BookKey       = FixedString<16>;  // "XSHG+600030"

enum class Side { BUY, SELL, UNKNOWN };

inline std::string to_string(Side s) {
    switch (s) {
    case Side::BUY:
        return "B";
    case Side::SELL:
        return "S";
    case Side::UNKNOWN:
    default:
        throw std::runtime_error("Invalid Side value");
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
        throw std::runtime_error("Invalid Market value");
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
    OrderId clOrderId;
    Market market;
    SecurityId securityId;
    Side side;
    uint64_t price;
    uint32_t qty;
    ShareholderId shareholderId;
};

inline void from_json(const nlohmann::json &j, Order &o) {
    o.clOrderId = j.at("clOrderId").get<std::string>();
    o.market = market_from_string(j.at("market").get<std::string>());
    o.securityId = j.at("securityId").get<std::string>();
    o.side = side_from_string(j.at("side").get<std::string>());
    double p;
    j.at("price").get_to(p);

    if (p <= 0.0) {
        throw std::invalid_argument("price must be positive, got: " + std::to_string(p));
    }
    
    o.price = static_cast<uint64_t>(std::round(p * 10000.0));
    j.at("qty").get_to(o.qty);
    o.shareholderId = j.at("shareholderId").get<std::string>();
    if (o.qty == 0) {
        throw std::invalid_argument("qty must be positive");
    }
    if (o.side == Side::BUY && o.qty % 100 != 0) {
        throw std::invalid_argument("buy qty must be a multiple of 100, got: " +
                                    std::to_string(o.qty));
    }
}

inline void to_json(nlohmann::json &j, const Order &o) {
    j["clOrderId"] = std::string(o.clOrderId);
    j["market"] = to_string(o.market);
    j["securityId"] = std::string(o.securityId);
    j["side"] = to_string(o.side);
    j["price"] = static_cast<double>(o.price) / 10000.0;
    j["qty"] = o.qty;
    j["shareholderId"] = std::string(o.shareholderId);
}

// 3.2 交易撤单
struct CancelOrder {
    OrderId clOrderId;
    OrderId origClOrderId;
    Market market;
    SecurityId securityId;
    ShareholderId shareholderId;
    Side side;
};

inline void from_json(const nlohmann::json &j, CancelOrder &o) {
    o.clOrderId = j.at("clOrderId").get<std::string>();
    o.origClOrderId = j.at("origClOrderId").get<std::string>();
    o.market = market_from_string(j.at("market").get<std::string>());
    o.securityId = j.at("securityId").get<std::string>();
    o.shareholderId = j.at("shareholderId").get<std::string>();
    o.side = side_from_string(j.at("side").get<std::string>());
}

// 3.3 行情信息
// 对于某个市场、某个证券代码的最新行情数据
struct MarketData {
    uint64_t bidPrice;
    uint64_t askPrice;
};

// 3.4 - 3.8 输出结构体（可以统一也可以分开）
struct OrderResponse {
    OrderId clOrderId;
    Market market;
    SecurityId securityId;
    Side side;
    uint32_t qty;
    uint64_t price;
    ShareholderId shareholderId;

    // 拒绝信息
    int32_t rejectCode = 0;
    std::string rejectText;

    // 成交信息
    ExecId execId;
    uint32_t execQty = 0;
    uint64_t execPrice = 0;

    // 类型
    enum Type { CONFIRM, REJECT, EXECUTION } type;
};

struct CancelResponse {
    OrderId clOrderId;
    OrderId origClOrderId;
    Market market;
    SecurityId securityId;
    ShareholderId shareholderId;
    Side side;

    // 确认信息
    uint32_t qty = 0;
    uint64_t price = 0;
    uint32_t cumQty = 0;
    uint32_t canceledQty = 0;

    // 拒绝信息
    int32_t rejectCode = 0;
    std::string rejectText;

    enum Type { CONFIRM, REJECT } type;
};

} // namespace hdf
