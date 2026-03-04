#include "sharded_matching_engine.h"
#include <functional>

namespace hdf {

ShardedMatchingEngine::ShardedMatchingEngine(int numShards)
    : numShards_(numShards) {
    for (int i = 0; i < numShards_; ++i)
        shards_.emplace_back(std::make_unique<Shard>());
}

ShardedMatchingEngine::~ShardedMatchingEngine() { stop(); }

void ShardedMatchingEngine::setExecCallback(ExecCallback cb) {
    execCallback_ = std::move(cb);
}

// ── 路由 ─────────────────────────────────────────────────────
// 同一 (market, securityId) 恒定路由到同一分片，保证单证券撮合顺序。
int ShardedMatchingEngine::routeShard(const Order &order) const {
    const std::string key = to_string(order.market) + "+" + order.securityId;
    return static_cast<int>(std::hash<std::string>{}(key) %
                            static_cast<size_t>(numShards_));
}

// ── start / stop ─────────────────────────────────────────────
void ShardedMatchingEngine::start() {
    for (auto &sp : shards_) {
        Shard &shard = *sp;
        shard.running = true;
        shard.worker = std::thread([this, &shard] { workerLoop(shard); });
    }
}

void ShardedMatchingEngine::stop() {
    for (auto &sp : shards_) {
        {
            std::lock_guard<std::mutex> lk(sp->mutex);
            sp->running = false;
        }
        sp->cv.notify_one();
    }
    for (auto &sp : shards_) {
        if (sp->worker.joinable()) sp->worker.join();
    }
}

// ── submitOrder ──────────────────────────────────────────────
void ShardedMatchingEngine::submitOrder(const Order &order) {
    int idx = routeShard(order);
    Shard &shard = *shards_[idx];
    {
        std::lock_guard<std::mutex> lk(shard.mutex);
        shard.queue.push_back(order);
    }
    shard.cv.notify_one();
}

// ── totalQueueDepth ──────────────────────────────────────────
size_t ShardedMatchingEngine::totalQueueDepth() const {
    size_t total = 0;
    for (const auto &sp : shards_) {
        std::lock_guard<std::mutex> lk(sp->mutex);
        total += sp->queue.size();
    }
    return total;
}

// ── processOrder (helper) ─────────────────────────────────────
void ShardedMatchingEngine::processOrder(Shard &shard, const Order &order) {
    auto result = shard.engine.match(order);
    if (execCallback_) {
        for (const auto &exec : result.executions)
            execCallback_(exec, order.clOrderId);
    }
    if (result.remainingQty > 0) {
        Order remaining   = order;
        remaining.qty     = result.remainingQty;
        shard.engine.addOrder(remaining);
    }
}

// ── workerLoop ───────────────────────────────────────────────
// 批量 swap 模式：持锁期间只做 swap，处理在无锁状态下进行。
void ShardedMatchingEngine::workerLoop(Shard &shard) {
    while (true) {
        std::deque<Order> batch;
        {
            std::unique_lock<std::mutex> lk(shard.mutex);
            // 等待：队列非空，或者停止信号
            shard.cv.wait(lk, [&shard] {
                return !shard.running || !shard.queue.empty();
            });
            batch.swap(shard.queue);  // O(1) swap，最小化锁持有时间
        }

        // 无锁处理
        for (const Order &order : batch)
            processOrder(shard, order);

        // 检查退出条件：running=false 且队列已空
        {
            std::unique_lock<std::mutex> lk(shard.mutex);
            if (!shard.running && shard.queue.empty())
                break;
        }
    }
}

} // namespace hdf
