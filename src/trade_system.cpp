#include "trade_system.h"

namespace hdf {

TradeSystem::TradeSystem() { core_.setLogger(&logger_); }

TradeSystem::~TradeSystem() { stopEventLoop(); }

bool TradeSystem::enableLogging(const std::string &filePath) {
    return logger_.open(filePath);
}

void TradeSystem::disableLogging() { logger_.close(); }

TradeLogger &TradeSystem::logger() { return logger_; }

void TradeSystem::setSendToClient(SendToClient callback) {
    core_.setSendToClient(callback);
}

void TradeSystem::setSendToExchange(SendToExchange callback) {
    core_.setSendToExchange(callback);
}

void TradeSystem::setSendMarketData(SendMarketData callback) {
    core_.setSendMarketData(callback);
}

void TradeSystem::handleOrder(const nlohmann::json &input) {
    core_.handleOrder(input);
}

void TradeSystem::handleCancel(const nlohmann::json &input) {
    core_.handleCancel(input);
}

void TradeSystem::handleMarketData(const nlohmann::json &input) {
    core_.handleMarketData(input);
}

void TradeSystem::handleResponse(const nlohmann::json &input) {
    core_.handleResponse(input);
}

nlohmann::json TradeSystem::queryOrderbook() const {
    return core_.queryOrderbook();
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
