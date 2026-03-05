#include "trade_system.h"

namespace hdf {

TradeSystem::TradeSystem() : cores_(1) {
    for (auto &core : cores_) {
        core.setLogger(&logger_);
    }
}

TradeSystem::~TradeSystem() { stopEventLoop(); }

bool TradeSystem::enableLogging(const std::string &filePath) {
    return logger_.open(filePath);
}

void TradeSystem::disableLogging() { logger_.close(); }

TradeLogger &TradeSystem::logger() { return logger_; }

void TradeSystem::setSendToClient(SendToClient callback) {
    for (auto &core : cores_) {
        core.setSendToClient(callback);
    }
}

void TradeSystem::setSendToExchange(SendToExchange callback) {
    for (auto &core : cores_) {
        core.setSendToExchange(callback);
    }
}

void TradeSystem::setSendMarketData(SendMarketData callback) {
    for (auto &core : cores_) {
        core.setSendMarketData(callback);
    }
}

// ─── 路由 ────────────────────────────────────────────────────

std::string TradeSystem::makeRouteKey(const std::string &market,
                                      const std::string &securityId) {
    return market + "+" + securityId;
}

SecurityCore &TradeSystem::coreFor(const std::string &market,
                                   const std::string &securityId) {
    size_t h = std::hash<std::string>{}(makeRouteKey(market, securityId));
    return cores_[h % cores_.size()];
}

const SecurityCore &TradeSystem::coreFor(const std::string &market,
                                         const std::string &securityId) const {
    size_t h = std::hash<std::string>{}(makeRouteKey(market, securityId));
    return cores_[h % cores_.size()];
}

// ─── 业务处理 ────────────────────────────────────────────────

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
    if (cores_.size() == 1) {
        cores_[0].handleMarketData(input);
        return;
    }
    // 按 core 分组
    std::vector<nlohmann::json> batches(cores_.size(),
                                        nlohmann::json::array());
    for (const auto &item : input) {
        std::string m = item.value("market", "");
        std::string s = item.value("securityId", "");
        size_t h = std::hash<std::string>{}(makeRouteKey(m, s));
        batches[h % cores_.size()].push_back(item);
    }
    for (size_t i = 0; i < cores_.size(); ++i) {
        if (!batches[i].empty()) {
            cores_[i].handleMarketData(batches[i]);
        }
    }
}

void TradeSystem::handleResponse(const nlohmann::json &input) {
    coreFor(input.value("market", ""), input.value("securityId", ""))
        .handleResponse(input);
}

nlohmann::json TradeSystem::queryOrderbook() const {
    if (cores_.size() == 1) {
        return cores_[0].queryOrderbook();
    }
    nlohmann::json merged;
    merged["bids"] = nlohmann::json::array();
    merged["asks"] = nlohmann::json::array();
    for (const auto &core : cores_) {
        auto snap = core.queryOrderbook();
        for (const auto &b : snap["bids"])
            merged["bids"].push_back(b);
        for (const auto &a : snap["asks"])
            merged["asks"].push_back(a);
    }
    return merged;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// MPSC 命令队列实现
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void TradeSystem::enqueueCommand(Command cmd) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        commandQueue_.push_back(std::move(cmd));
    }
    queueCv_.notify_one();
}

void TradeSystem::submitOrder(const nlohmann::json &input) {
    enqueueCommand(CmdOrder{input});
}

void TradeSystem::submitCancel(const nlohmann::json &input) {
    enqueueCommand(CmdCancel{input});
}

void TradeSystem::submitResponse(const nlohmann::json &input) {
    enqueueCommand(CmdResponse{input});
}

void TradeSystem::submitMarketData(const nlohmann::json &input) {
    enqueueCommand(CmdMarketData{input});
}

size_t TradeSystem::queueDepth() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return commandQueue_.size();
}

void TradeSystem::dispatchCommand(Command &cmd) {
    std::visit(
        [this](auto &c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, CmdOrder>) {
                handleOrder(c.input);
            } else if constexpr (std::is_same_v<T, CmdCancel>) {
                handleCancel(c.input);
            } else if constexpr (std::is_same_v<T, CmdResponse>) {
                handleResponse(c.input);
            } else if constexpr (std::is_same_v<T, CmdMarketData>) {
                handleMarketData(c.input);
            }
        },
        cmd);
}

void TradeSystem::eventLoop() {
    while (true) {
        std::deque<Command> batch;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] {
                return !commandQueue_.empty() || !eventLoopRunning_;
            });
            // 取出所有待处理命令（批量 swap，减少锁持有时间）
            batch.swap(commandQueue_);
        }
        // 在无锁状态下逐条处理
        for (auto &cmd : batch) {
            dispatchCommand(cmd);
        }
        // 队列为空且已请求停止 → 退出
        if (!eventLoopRunning_) {
            // 再检查一次，防止 stop 和 enqueue 同时发生遗漏
            std::lock_guard<std::mutex> lock(queueMutex_);
            for (auto &cmd : commandQueue_) {
                dispatchCommand(cmd);
            }
            commandQueue_.clear();
            break;
        }
    }
}

void TradeSystem::startEventLoop() {
    if (eventLoopRunning_)
        return;
    eventLoopRunning_ = true;
    eventLoopThread_ = std::thread(&TradeSystem::eventLoop, this);
}

void TradeSystem::stopEventLoop() {
    if (!eventLoopRunning_)
        return;
    eventLoopRunning_ = false;
    queueCv_.notify_one();
    if (eventLoopThread_.joinable()) {
        eventLoopThread_.join();
    }
}

} // namespace hdf
