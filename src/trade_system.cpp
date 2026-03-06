#include "trade_system.h"
#include "constants.h"
#include "types.h"

namespace hdf {

TradeSystem::TradeSystem(size_t numBuckets) : buckets_() {
    const size_t N = (numBuckets > 0)
                         ? numBuckets
                         : std::max(1u, std::thread::hardware_concurrency());
    buckets_.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        buckets_.push_back(std::make_unique<WorkerBucket>());
        buckets_.back()->core.setLogger(&logger_);
    }
}

TradeSystem::~TradeSystem() { stopEventLoop(); }

bool TradeSystem::enableLogging(const std::string &filePath) {
    return logger_.open(filePath);
}

void TradeSystem::disableLogging() { logger_.close(); }

TradeLogger &TradeSystem::logger() { return logger_; }

void TradeSystem::setSendToClient(SendToClient callback) {
    sendToClient_ = callback;
    for (auto &b : buckets_) {
        b->core.setSendToClient(callback);
    }
}

void TradeSystem::setSendToExchange(SendToExchange callback) {
    for (auto &b : buckets_) {
        b->core.setSendToExchange(callback);
    }
}

void TradeSystem::setSendMarketData(SendMarketData callback) {
    for (auto &b : buckets_) {
        b->core.setSendMarketData(callback);
    }
}

// ─── 路由 ────────────────────────────────────────────────────

std::string TradeSystem::makeRouteKey(const std::string &market,
                                      const std::string &securityId) {
    return market + "+" + securityId;
}

size_t TradeSystem::routeIndex(const std::string &market,
                               const std::string &securityId) const {
    size_t h = std::hash<std::string>{}(makeRouteKey(market, securityId));
    return h % buckets_.size();
}

SecurityCore &TradeSystem::coreFor(const std::string &market,
                                   const std::string &securityId) {
    return buckets_[routeIndex(market, securityId)]->core;
}

const SecurityCore &TradeSystem::coreFor(const std::string &market,
                                         const std::string &securityId) const {
    return buckets_[routeIndex(market, securityId)]->core;
}

// ─── 同步业务处理（非线程安全，直接调用） ──────────────────────

void TradeSystem::handleOrder(const nlohmann::json &input) {
    coreFor(input.value("market", ""), input.value("securityId", ""))
        .handleOrder(input);
}

void TradeSystem::handleCancel(const nlohmann::json &input) {
    coreFor(input.value("market", ""), input.value("securityId", ""))
        .handleCancel(input);
}

void TradeSystem::handleMarketData(const nlohmann::json &input) {
    if (!input.is_array())
        return;
    if (buckets_.size() == 1) {
        buckets_[0]->core.handleMarketData(input);
        return;
    }
    std::vector<nlohmann::json> batches(buckets_.size(),
                                        nlohmann::json::array());
    for (const auto &item : input) {
        std::string m = item.value("market", "");
        std::string s = item.value("securityId", "");
        batches[routeIndex(m, s)].push_back(item);
    }
    for (size_t i = 0; i < buckets_.size(); ++i) {
        if (!batches[i].empty()) {
            buckets_[i]->core.handleMarketData(batches[i]);
        }
    }
}

void TradeSystem::handleResponse(const nlohmann::json &input) {
    coreFor(input.value("market", ""), input.value("securityId", ""))
        .handleResponse(input);
}

nlohmann::json TradeSystem::queryOrderbook() const {
    if (buckets_.size() == 1) {
        return buckets_[0]->core.queryOrderbook();
    }
    nlohmann::json merged;
    merged["bids"] = nlohmann::json::array();
    merged["asks"] = nlohmann::json::array();
    for (const auto &b : buckets_) {
        auto snap = b->core.queryOrderbook();
        for (const auto &bid : snap["bids"])
            merged["bids"].push_back(bid);
        for (const auto &ask : snap["asks"])
            merged["asks"].push_back(ask);
    }
    return merged;
}

nlohmann::json TradeSystem::queryOrderbook(const std::string &securityId,
                                           Market market) const {
    return coreFor(to_string(market), securityId)
        .queryOrderbook(securityId, market);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 每 WorkerBucket 独立队列实现
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void TradeSystem::enqueueToWorker(size_t idx, Command cmd) {
    auto &bucket = *buckets_[idx];
    {
        std::lock_guard<std::mutex> lock(bucket.mutex);
        bucket.taskQueue.push_back(std::move(cmd));
    }
    bucket.cv.notify_one();
}

void TradeSystem::submitOrder(const nlohmann::json &input) {
    try {
        Order order = input.get<Order>();
        size_t idx = routeIndex(to_string(order.market), order.securityId);
        enqueueToWorker(idx, CmdOrder{std::move(order)});
    } catch (const std::exception &e) {
        if (sendToClient_) {
            nlohmann::json response;
            response["clOrderId"] = input.value("clOrderId", "");
            response["rejectCode"] = ORDER_INVALID_FORMAT_REJECT_CODE;
            response["rejectText"] =
                std::string(ORDER_INVALID_FORMAT_REJECT_REASON) + ": " + e.what();
            sendToClient_(response);
        }
    }
}

void TradeSystem::submitCancel(const nlohmann::json &input) {
    try {
        CancelOrder order = input.get<CancelOrder>();
        size_t idx = routeIndex(to_string(order.market), order.securityId);
        enqueueToWorker(idx, CmdCancel{std::move(order)});
    } catch (const std::exception &e) {
        if (sendToClient_) {
            nlohmann::json response;
            response["clOrderId"] = input.value("clOrderId", "");
            response["rejectCode"] = ORDER_INVALID_FORMAT_REJECT_CODE;
            response["rejectText"] =
                std::string(ORDER_INVALID_FORMAT_REJECT_REASON) + ": " + e.what();
            sendToClient_(response);
        }
    }
}

void TradeSystem::submitResponse(const nlohmann::json &input) {
    enqueueToWorker(
        routeIndex(input.value("market", ""), input.value("securityId", "")),
        CmdResponse{input});
}

void TradeSystem::submitMarketData(const nlohmann::json &input) {
    if (!input.is_array())
        return;
    if (buckets_.size() == 1) {
        enqueueToWorker(0, CmdMarketData{input});
        return;
    }
    std::vector<nlohmann::json> batches(buckets_.size(),
                                        nlohmann::json::array());
    for (const auto &item : input) {
        std::string m = item.value("market", "");
        std::string s = item.value("securityId", "");
        batches[routeIndex(m, s)].push_back(item);
    }
    for (size_t i = 0; i < buckets_.size(); ++i) {
        if (!batches[i].empty()) {
            enqueueToWorker(i, CmdMarketData{std::move(batches[i])});
        }
    }
}

size_t TradeSystem::queueDepth() const {
    size_t total = 0;
    for (const auto &b : buckets_) {
        std::lock_guard<std::mutex> lock(b->mutex);
        total += b->taskQueue.size();
    }
    return total;
}

void TradeSystem::dispatchCommand(WorkerBucket &bucket, Command &cmd) {
    std::visit(
        [&bucket](auto &c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, CmdOrder>) {
                bucket.core.handleOrder(std::move(c.order));
            } else if constexpr (std::is_same_v<T, CmdCancel>) {
                bucket.core.handleCancel(std::move(c.order));
            } else if constexpr (std::is_same_v<T, CmdResponse>) {
                bucket.core.handleResponse(c.input);
            } else if constexpr (std::is_same_v<T, CmdMarketData>) {
                bucket.core.handleMarketData(c.input);
            }
        },
        cmd);
}

void TradeSystem::workerLoop(WorkerBucket *bucket) {
    while (true) {
        std::deque<Command> batch;
        {
            std::unique_lock<std::mutex> lock(bucket->mutex);
            bucket->cv.wait(lock, [bucket] {
                return !bucket->taskQueue.empty() || !bucket->running;
            });
            batch.swap(bucket->taskQueue);
        }
        for (auto &cmd : batch) {
            dispatchCommand(*bucket, cmd);
        }
        if (!bucket->running) {
            std::lock_guard<std::mutex> lock(bucket->mutex);
            for (auto &cmd : bucket->taskQueue) {
                dispatchCommand(*bucket, cmd);
            }
            bucket->taskQueue.clear();
            break;
        }
    }
}

void TradeSystem::startEventLoop() {
    for (auto &b : buckets_) {
        if (b->running)
            continue;
        b->running = true;
        b->thread = std::thread(&TradeSystem::workerLoop, this, b.get());
    }
}

void TradeSystem::stopEventLoop() {
    for (auto &b : buckets_) {
        if (!b->running)
            continue;
        b->running = false;
        b->cv.notify_one();
    }
    for (auto &b : buckets_) {
        if (b->thread.joinable()) {
            b->thread.join();
        }
    }
}

} // namespace hdf
