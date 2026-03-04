#pragma once

#include "types.h"
#include <optional>
#include <vector>
#include <string>
#include <unordered_map>
#include <map>
#include <list>
#include <memory>
#include <cmath>

namespace hdf {

// 辅助结构：单个证券的订单簿
struct SecurityOrderBook {
    // 买盘: Price -> Orders 
    // double 作为 key 有精度风险。生产环境建议转为 int64_t (price * 10000)。
    // 这里为了适配 types.h 直接使用 double，依赖输入价格的规范性。
    std::map<double, std::list<Order>> bids;
    
    // 卖盘: Price -> Orders
    std::map<double, std::list<Order>> asks;

    // 订单索引：clOrderId -> 位置信息 (用于 O(1) 查找撤单/减仓)
    struct OrderLocation {
        Side side;
        double price;
        std::list<Order>::iterator it;
    };
    std::unordered_map<std::string, OrderLocation> orderIndex;
};

class MatchingEngine {
  public:
    MatchingEngine();
    ~MatchingEngine();

    struct MatchResult {
        std::vector<OrderResponse> executions; 
        uint32_t remainingQty = 0;
    };

    std::optional<MatchResult>
    match(const Order &order,
          const std::optional<MarketData> &marketData = std::nullopt);

    void addOrder(const Order &order);

    CancelResponse cancelOrder(const std::string &clOrderId);

    void reduceOrderQty(const std::string &clOrderId, uint32_t qty);

  private:
    // 生成唯一Key: "Market:SecurityId"
    static std::string makeBookKey(const std::string& market, const std::string& securityId);

    // 1. 分片存储：每个证券独立的订单簿
    std::unordered_map<std::string, SecurityOrderBook> books_;

    // 2. 全局索引：直接从 clOrderId 定位到具体的 Book Key 和内部位置
    // 这是实现 O(1) 撤单的关键，避免遍历所有证券
    struct GlobalLoc {
        std::string bookKey;
        Side side;
        double price;
        std::list<Order>::iterator it;
    };
    std::unordered_map<std::string, GlobalLoc> globalOrderIndex_;

    void executeMatch(const Order& incomingOrder, 
                      SecurityOrderBook& book, 
                      const std::string& bookKey,
                      MatchResult& result);
                      
    // 辅助：从全局索引和局部索引中移除订单
    void removeOrderFromIndex(const std::string& clOrderId, const std::string& bookKey);
};

} // namespace hdf