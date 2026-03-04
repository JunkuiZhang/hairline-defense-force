#pragma once

#include "matching_engine.h"
#include "types.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hdf {

/**
 * @brief 分片撮合引擎 — 按 (market, securityId) 将订单路由到独立分片。
 *
 * 架构：
 *   N 个 Shard，每个 Shard 持有：
 *     - 独立的 MatchingEngine（只处理路由到本分片的证券）
 *     - 独立的 MPSC 命令队列（mutex + deque）
 *     - 独立的工作线程（串行处理，无跨分片竞争）
 *
 *   路由策略：hash(to_string(market) + securityId) % numShards
 *   同一 (market, securityId) 的所有订单始终落在同一分片，
 *   保证单证券的撮合顺序一致性，同时消除订单簿内的 securityId 过滤开销。
 *
 * 性能目标：
 *   理论吞吐上限 ≈ numShards × 单线程吞吐（无共享状态时线性扩展）
 *
 * @note 线程安全：submitOrder() 可从任意线程调用。
 *       start() / stop() 不是线程安全的，应在单线程中管理生命周期。
 */
class ShardedMatchingEngine {
  public:
    /**
     * @brief 每笔成交回调。在对应的 Shard 工作线程中调用，需保证回调线程安全。
     *
     * @param exec  成交回报（被动方信息及成交细节）
     * @param activeOrderId  主动方订单 clOrderId
     */
    using ExecCallback =
        std::function<void(const OrderResponse &exec,
                           const std::string &activeOrderId)>;

    /**
     * @param numShards  分片数，建议 = 硬件线程数（或证券种类数，取较小值）
     */
    explicit ShardedMatchingEngine(int numShards);
    ~ShardedMatchingEngine();

    // 禁止拷贝
    ShardedMatchingEngine(const ShardedMatchingEngine &) = delete;
    ShardedMatchingEngine &operator=(const ShardedMatchingEngine &) = delete;

    /**
     * @brief 设置成交回调（在 start() 前调用）。
     */
    void setExecCallback(ExecCallback cb);

    /**
     * @brief 启动所有分片工作线程。
     */
    void start();

    /**
     * @brief 停止所有工作线程，等待队列排空后返回。
     */
    void stop();

    /**
     * @brief 线程安全地将订单路由到对应分片队列。
     *
     * 路由依据：hash(to_string(order.market) + order.securityId) % numShards
     */
    void submitOrder(const Order &order);

    /**
     * @brief 返回所有分片队列的总积压深度（监控用）。
     */
    size_t totalQueueDepth() const;

    /**
     * @brief 返回分片数（只读）。
     */
    int numShards() const { return numShards_; }

  private:
    int numShards_;
    ExecCallback execCallback_;

    struct Shard {
        MatchingEngine engine;
        std::deque<Order> queue;
        mutable std::mutex mutex;
        std::condition_variable cv;
        std::thread worker;
        std::atomic<bool> running{false};
    };

    std::vector<std::unique_ptr<Shard>> shards_;

    /**
     * @brief 计算订单应路由到的分片索引。
     */
    int routeShard(const Order &order) const;

    /**
     * @brief 各分片的工作线程主循环。
     */
    void workerLoop(Shard &shard);
    void processOrder(Shard &shard, const Order &order);
};

} // namespace hdf
