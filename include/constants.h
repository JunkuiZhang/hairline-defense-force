#pragma once

#include <cstdint>

namespace hdf {

constexpr int32_t ORDER_CROSS_TRADE_REJECT_CODE = 0x01;
constexpr const char *ORDER_CROSS_TRADE_REJECT_REASON = "Cross trade detected";

constexpr int32_t ORDER_INVALID_FORMAT_REJECT_CODE = 0x02;
constexpr const char *ORDER_INVALID_FORMAT_REJECT_REASON =
    "Invalid order format";

} // namespace hdf
