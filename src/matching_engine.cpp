#include "matching_engine.h"
#include "types.h"

namespace hdf {

MatchingEngine::MatchingEngine() {}

MatchingEngine::~MatchingEngine() {}

std::optional<MatchingEngine::MatchResult>
MatchingEngine::match(const Order &order,
                      const std::optional<MarketData> &marketData) {
    // TODO: 实现撮合逻辑
    return std::nullopt;
}

void MatchingEngine::addOrder(const Order &order) {
    // TODO: 此函数为参考实现，可以随意删除更改
}

CancelResponse MatchingEngine::cancelOrder(const std::string &clOrderId) {
    // TODO: 此函数为参考实现，可以随意删除更改
    CancelResponse response;
    // response.clOrderId = ...;
    response.origClOrderId = clOrderId;
    // response.market = ...;
    // response.securityId = ...;
    // response.shareholderId = ...;
    // response.side = ...;
    // response.qty = ...;
    // response.price = ...;
    // response.cumQty = ...;
    // response.canceledQty = ...;
    return response;
}

} // namespace hdf
