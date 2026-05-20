#include "trade_system.h"
#include "constants.h"
#include "types.h"
#include "utils.h"
#include <emmintrin.h>

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

TradeSystem::TradeSystem(const std::vector<int> &cores) {
    size_t numBuckets = cores.empty() ? 1 : cores.size();

    for (size_t i = 0; i < numBuckets; ++i) {
        auto bucket = std::make_unique<WorkerBucket>();
        bucket->coreId = cores.empty() ? -1 : cores[i];
        bucket->core.setLogger(&logger_);
        buckets_.push_back(std::move(bucket));
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

size_t TradeSystem::routeIndex(Market market,
                               const SecurityId &securityId) const {
    BookKey key = makeRouteKey(market, securityId);
    size_t h = std::hash<BookKey>{}(key);
    return h % buckets_.size();
}

size_t TradeSystem::routeIndex(const std::string &market,
                               const SecurityId &securityId) const {
    BookKey key = makeRouteKey(market, securityId);
    size_t h = std::hash<BookKey>{}(key);
    return h % buckets_.size();
}

// ─── 同步业务处理（非线程安全，直接调用） ──────────────────────

void TradeSystem::handleOrder(const nlohmann::json &input) {
    std::string market = input.value("market", "");
    SecurityId secId = input.value("securityId", "");
    buckets_[routeIndex(market, secId)]->core.handleOrder(input);
}

void TradeSystem::handleCancel(const nlohmann::json &input) {
    std::string market = input.value("market", "");
    SecurityId secId = input.value("securityId", "");
    buckets_[routeIndex(market, secId)]->core.handleCancel(input);
}

void TradeSystem::handleMarketData(const std::vector<MarketDataItem> &items) {
    if (items.empty())
        return;
    if (buckets_.size() == 1) {
        buckets_[0]->core.handleMarketData(items);
        return;
    }
    // 按 bucket 分组
    std::vector<std::vector<MarketDataItem>> batches(buckets_.size());
    for (const auto &item : items) {
        size_t idx = routeIndex(item.market, item.securityId);
        batches[idx].push_back(item);
    }
    for (size_t i = 0; i < buckets_.size(); ++i) {
        if (!batches[i].empty()) {
            buckets_[i]->core.handleMarketData(batches[i]);
        }
    }
}

void TradeSystem::handleResponse(const ExchangeReport &report) {
    buckets_[routeIndex(report.market, report.securityId)]->core.handleResponse(
        report);
}

nlohmann::json TradeSystem::queryOrderbook() {
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

nlohmann::json TradeSystem::queryOrderbook(const SecurityId &securityId,
                                           Market market) {
    return buckets_[routeIndex(market, securityId)]->core.queryOrderbook(
        securityId, market);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 每 WorkerBucket 独立队列实现
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void TradeSystem::enqueueToWorker(size_t idx, Command cmd) {
    auto &bucket = *buckets_[idx];
    while (!bucket.taskQueue.push(std::move(cmd))) {
        _mm_pause();
    }
}

void TradeSystem::submitOrder(const nlohmann::json &input) {
    try {
        Order order = input.get<Order>();
        submitOrder(std::move(order));
    } catch (const std::exception &e) {
        if (sendToClient_) {
            OrderResponse resp;
            resp.clOrderId = input.value("clOrderId", "");
            resp.rejectCode = ORDER_INVALID_FORMAT_REJECT_CODE;
            std::string msg = std::string(ORDER_INVALID_FORMAT_REJECT_REASON) +
                              ": " + e.what();
            resp.rejectText = msg;
            resp.type = OrderResponse::REJECT;
            sendToClient_(resp);
        }
    }
}

void TradeSystem::submitOrder(Order order) {
    size_t idx = routeIndex(order.market, order.securityId);
    enqueueToWorker(idx, CmdOrder{std::move(order)});
}

void TradeSystem::submitCancel(const nlohmann::json &input) {
    try {
        CancelOrder order = input.get<CancelOrder>();
        submitCancel(std::move(order));
    } catch (const std::exception &e) {
        if (sendToClient_) {
            CancelResponse resp;
            resp.clOrderId = input.value("clOrderId", "");
            resp.rejectCode = ORDER_INVALID_FORMAT_REJECT_CODE;
            std::string msg = std::string(ORDER_INVALID_FORMAT_REJECT_REASON) +
                              ": " + e.what();
            resp.rejectText = msg;
            resp.type = CancelResponse::REJECT;
            sendToClient_(resp);
        }
    }
}

void TradeSystem::submitCancel(CancelOrder order) {
    size_t idx = routeIndex(order.market, order.securityId);
    enqueueToWorker(idx, CmdCancel{std::move(order)});
}

void TradeSystem::submitResponse(const ExchangeReport &report) {
    size_t idx = routeIndex(report.market, report.securityId);
    enqueueToWorker(idx, CmdResponse{report});
}

void TradeSystem::submitMarketData(const std::vector<MarketDataItem> &items) {
    if (items.empty())
        return;
    if (buckets_.size() == 1) {
        // 单 bucket：直接打包
        for (size_t i = 0; i < items.size(); i += 8) {
            CmdMarketData cmd;
            cmd.count =
                static_cast<uint8_t>(std::min(size_t(8), items.size() - i));
            for (uint8_t j = 0; j < cmd.count; ++j) {
                cmd.items[j] = items[i + j];
            }
            enqueueToWorker(0, cmd);
        }
        return;
    }
    // 多 bucket：按路由分组
    std::vector<std::vector<MarketDataItem>> batches(buckets_.size());
    for (const auto &item : items) {
        batches[routeIndex(item.market, item.securityId)].push_back(item);
    }
    for (size_t i = 0; i < buckets_.size(); ++i) {
        auto &batch = batches[i];
        for (size_t j = 0; j < batch.size(); j += 8) {
            CmdMarketData cmd;
            cmd.count =
                static_cast<uint8_t>(std::min(size_t(8), batch.size() - j));
            for (uint8_t k = 0; k < cmd.count; ++k) {
                cmd.items[k] = batch[j + k];
            }
            enqueueToWorker(i, cmd);
        }
    }
}

size_t TradeSystem::queueDepth() const {
    size_t total = 0;
    for (const auto &b : buckets_) {
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
                bucket.core.handleResponse(c.report);
            } else if constexpr (std::is_same_v<T, CmdMarketData>) {
                bucket.core.handleMarketData(c.items, c.count);
            }
        },
        cmd);
}

void TradeSystem::workerLoop(WorkerBucket *bucket) {
    if (bucket->coreId >= 0) {
        hdf::pin_to_core(bucket->coreId);
    }

    while (true) {
        Command cmd;
        if (bucket->taskQueue.pop(cmd)) {
            dispatchCommand(*bucket, cmd);
            while (bucket->taskQueue.pop(cmd)) {
                dispatchCommand(*bucket, cmd);
            }
        } else {
            if (!bucket->running.load(std::memory_order_relaxed))
                break;
            _mm_pause();
        }
    }
    Command cmd;
    while (bucket->taskQueue.pop(cmd)) {
        dispatchCommand(*bucket, cmd);
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
    }
    for (auto &b : buckets_) {
        if (b->thread.joinable()) {
            b->thread.join();
        }
    }
}

} // namespace hdf
