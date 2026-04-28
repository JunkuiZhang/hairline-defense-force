#pragma once

#include "fixed_str.h"
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

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
        return "?";
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
        return "?";
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

// ─── 常用定长字符串类型别名 ─────────────────────────────

using OrderId = FixedStr<24>;
using SecurityId = FixedStr<12>;
using ShareholderId = FixedStr<16>;
using ExecIdStr = FixedStr<24>;
using RejectText = FixedStr<128>;
using BookKey = FixedStr<24>;  // "XSHG+600030"
using RiskKey = FixedStr<48>; // "SH0001_0_600000"

// ─── 路由 key 生成 ─────────────────────────────────────

inline BookKey makeRouteKey(Market market, const SecurityId &securityId) {
    BookKey key(to_string(market));
    key.append("+");
    key.append(std::string_view(securityId));
    return key;
}

inline BookKey makeRouteKey(const std::string &market,
                            const SecurityId &securityId) {
    BookKey key(market);
    key.append("+");
    key.append(std::string_view(securityId));
    return key;
}

// ============================================================
// 3.1 交易订单
// ============================================================
struct Order {
    OrderId clOrderId;
    Market market;
    SecurityId securityId;
    Side side;
    double price;
    uint32_t qty;
    ShareholderId shareholderId;

    std::optional<size_t> prev;
    std::optional<size_t> next;

    // 订单簿内部状态（不序列化）
    uint32_t remainingQty = 0;
    uint32_t cumQty = 0;
};

inline void from_json(const nlohmann::json &j, Order &o) {
    o.clOrderId = j.at("clOrderId").get<std::string>();
    o.market = market_from_string(j.at("market").get<std::string>());
    o.securityId = j.at("securityId").get<std::string>();
    o.side = side_from_string(j.at("side").get<std::string>());
    j.at("price").get_to(o.price);
    j.at("qty").get_to(o.qty);
    o.shareholderId = j.at("shareholderId").get<std::string>();

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

inline void to_json(nlohmann::json &j, const Order &o) {
    j["clOrderId"] = o.clOrderId.str();
    j["market"] = to_string(o.market);
    j["securityId"] = o.securityId.str();
    j["side"] = to_string(o.side);
    j["price"] = o.price;
    j["qty"] = o.qty;
    j["shareholderId"] = o.shareholderId.str();
}

// ============================================================
// 3.2 交易撤单
// ============================================================
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

inline void to_json(nlohmann::json &j, const CancelOrder &o) {
    j["clOrderId"] = o.clOrderId.str();
    j["origClOrderId"] = o.origClOrderId.str();
    j["market"] = to_string(o.market);
    j["securityId"] = o.securityId.str();
    j["shareholderId"] = o.shareholderId.str();
    j["side"] = to_string(o.side);
}

// ============================================================
// 3.3 行情信息
// ============================================================
struct MarketData {
    double bidPrice;
    double askPrice;
};

// ============================================================
// 行情条目（用于推送和队列传递）
// ============================================================
struct MarketDataItem {
    Market market;
    SecurityId securityId;
    double bidPrice;
    double askPrice;
};

// ============================================================
// 3.4 - 3.8 输出结构体
// ============================================================
struct OrderResponse {
    OrderId clOrderId;
    Market market;
    SecurityId securityId;
    Side side;
    uint32_t qty;
    double price;
    ShareholderId shareholderId;

    // 拒绝信息
    int32_t rejectCode = 0;
    RejectText rejectText;

    // 成交信息
    ExecIdStr execId;
    uint32_t execQty = 0;
    double execPrice = 0.0;

    // 类型
    enum Type { CONFIRM, REJECT, EXECUTION } type;
};

inline void to_json(nlohmann::json &j, const OrderResponse &o) {
    j["clOrderId"] = o.clOrderId.str();
    j["market"] = to_string(o.market);
    j["securityId"] = o.securityId.str();
    j["side"] = to_string(o.side);
    j["qty"] = o.qty;
    j["price"] = o.price;
    j["shareholderId"] = o.shareholderId.str();
    if (o.rejectCode != 0) {
        j["rejectCode"] = o.rejectCode;
        j["rejectText"] = o.rejectText.str();
    }
    if (!o.execId.empty()) {
        j["execId"] = o.execId.str();
        j["execQty"] = o.execQty;
        j["execPrice"] = o.execPrice;
    }
}

struct CancelResponse {
    OrderId clOrderId;
    OrderId origClOrderId;
    Market market;
    SecurityId securityId;
    ShareholderId shareholderId;
    Side side;

    // 确认信息
    uint32_t qty = 0;
    double price = 0.0;
    uint32_t cumQty = 0;
    uint32_t canceledQty = 0;

    // 拒绝信息
    int32_t rejectCode = 0;
    RejectText rejectText;

    enum Type { CONFIRM, REJECT } type;
};

inline void to_json(nlohmann::json &j, const CancelResponse &o) {
    j["clOrderId"] = o.clOrderId.str();
    j["origClOrderId"] = o.origClOrderId.str();
    j["market"] = to_string(o.market);
    j["securityId"] = o.securityId.str();
    j["shareholderId"] = o.shareholderId.str();
    j["side"] = to_string(o.side);
    if (o.rejectCode != 0) {
        j["rejectCode"] = o.rejectCode;
        j["rejectText"] = o.rejectText.str();
    } else {
        j["qty"] = o.qty;
        j["price"] = o.price;
        j["cumQty"] = o.cumQty;
        j["canceledQty"] = o.canceledQty;
    }
}

// ============================================================
// 交易所回报（op3: 从交易所返回的数据）
// ============================================================
struct ExchangeReport {
    OrderId clOrderId;
    Market market;
    SecurityId securityId;
    Side side;
    uint32_t qty = 0;
    double price = 0.0;
    ShareholderId shareholderId;
    OrderId origClOrderId; // 撤单回报时有值

    // 成交信息
    ExecIdStr execId;
    uint32_t execQty = 0;
    double execPrice = 0.0;

    // 拒绝信息
    int32_t rejectCode = 0;
    RejectText rejectText;

    // 确认信息
    uint32_t cumQty = 0;
    uint32_t canceledQty = 0;
};

inline void from_json(const nlohmann::json &j, ExchangeReport &r) {
    r.clOrderId = j.value("clOrderId", "");
    if (j.contains("market"))
        r.market = market_from_string(j["market"].get<std::string>());
    r.securityId = j.value("securityId", "");
    if (j.contains("side"))
        r.side = side_from_string(j["side"].get<std::string>());
    r.qty = j.value("qty", 0u);
    r.price = j.value("price", 0.0);
    r.shareholderId = j.value("shareholderId", "");
    r.origClOrderId = j.value("origClOrderId", "");
    r.execId = j.value("execId", "");
    r.execQty = j.value("execQty", 0u);
    r.execPrice = j.value("execPrice", 0.0);
    r.rejectCode = j.value("rejectCode", 0);
    r.rejectText = j.value("rejectText", "");
    r.cumQty = j.value("cumQty", 0u);
    r.canceledQty = j.value("canceledQty", 0u);
}

inline void to_json(nlohmann::json &j, const ExchangeReport &r) {
    j["clOrderId"] = r.clOrderId.str();
    j["market"] = to_string(r.market);
    j["securityId"] = r.securityId.str();
    j["side"] = to_string(r.side);
    if (r.qty > 0)
        j["qty"] = r.qty;
    if (r.price > 0)
        j["price"] = r.price;
    if (!r.shareholderId.empty())
        j["shareholderId"] = r.shareholderId.str();
    if (!r.origClOrderId.empty())
        j["origClOrderId"] = r.origClOrderId.str();
    if (!r.execId.empty()) {
        j["execId"] = r.execId.str();
        j["execQty"] = r.execQty;
        j["execPrice"] = r.execPrice;
    }
    if (r.rejectCode != 0) {
        j["rejectCode"] = r.rejectCode;
        j["rejectText"] = r.rejectText.str();
    }
    if (r.cumQty > 0)
        j["cumQty"] = r.cumQty;
    if (r.canceledQty > 0)
        j["canceledQty"] = r.canceledQty;
}

// ============================================================
// 回调类型别名
// ============================================================
using ClientReport = std::variant<OrderResponse, CancelResponse>;
using ExchangeRequest = std::variant<Order, CancelOrder>;

/// 将 ClientReport variant 转为 JSON（供边界使用）
inline nlohmann::json to_json_report(const ClientReport &report) {
    return std::visit(
        [](const auto &r) -> nlohmann::json {
            nlohmann::json j;
            to_json(j, r);
            return j;
        },
        report);
}

/// 将 ExchangeRequest variant 转为 JSON（供边界使用）
inline nlohmann::json to_json_request(const ExchangeRequest &req) {
    return std::visit(
        [](const auto &r) -> nlohmann::json {
            nlohmann::json j;
            to_json(j, r);
            return j;
        },
        req);
}

} // namespace hdf
