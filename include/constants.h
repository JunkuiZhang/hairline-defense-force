#pragma once

#include <string>

namespace hdf {

const int32_t ORDER_CROSS_TRADE_REJECT_CODE = 0x01;
const std::string ORDER_CROSS_TRADE_REJECT_REASON = "Cross trade detected";

const int32_t ORDER_INVALID_FORMAT_REJECT_CODE = 0x02;
const std::string ORDER_INVALID_FORMAT_REJECT_REASON = "Invalid order format";

} // namespace hdf
