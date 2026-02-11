#include "matching_engine.h"

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

void MatchingEngine::cancelOrder(const std::string &clOrderId) {
    // TODO: 此函数为参考实现，可以随意删除更改
}

} // namespace hdf
