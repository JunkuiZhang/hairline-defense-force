#include "trade_logger.h"
#include <chrono>
#include <filesystem>
#include <iostream>

namespace hdf {

TradeLogger::TradeLogger() {}

TradeLogger::~TradeLogger() { close(); }

// ============================================================
// 文件管理
// ============================================================

bool TradeLogger::open(const std::string &filePath) {
    if (isOpen_)
        close();

    // 自动创建父目录
    auto parent = std::filesystem::path(filePath).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    file_.open(filePath, std::ios::app);
    if (!file_.is_open()) {
        std::cerr << "[TradeLogger] 无法打开文件: " << filePath << std::endl;
        return false;
    }

    filePath_ = filePath;
    stopRequested_ = false;
    isOpen_ = true;

    // 启动后台写入线程
    writerThread_ = std::thread(&TradeLogger::writerLoop, this);

    std::cout << "[TradeLogger] open: " << filePath << std::endl;
    return true;
}

void TradeLogger::close() {
    if (!isOpen_)
        return;

    // 通知后台线程停止
    stopRequested_ = true;
    cv_.notify_one();

    // 等待后台线程写完队列剩余内容并退出
    if (writerThread_.joinable()) {
        writerThread_.join();
    }

    file_.close();
    isOpen_ = false;
    std::cout << "[TradeLogger] close: " << filePath_ << std::endl;
}

bool TradeLogger::isOpen() const { return isOpen_; }

// ============================================================
// 异步队列
// ============================================================

void TradeLogger::enqueue(nlohmann::json record) {
    if (!isOpen_)
        return;

    // 添加毫秒级时间戳
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch())
                  .count();
    record["timestamp"] = ms;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(record));
    }
    cv_.notify_one();
}

void TradeLogger::writerLoop() {
    while (true) {
        std::queue<nlohmann::json> batch;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock,
                     [this] { return !queue_.empty() || stopRequested_; });
            std::swap(batch, queue_);
        }

        // 批量写入
        while (!batch.empty()) {
            file_ << batch.front().dump() << '\n';
            batch.pop();
        }
        file_.flush();

        // 队列已排空且收到停止信号时退出
        if (stopRequested_) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty())
                break;
        }
    }
}

// ============================================================
// 事件记录接口
// ============================================================

void TradeLogger::logOrderNew(const Order &order) {
    nlohmann::json record;
    record["event"] = "ORDER_NEW";
    record["clOrderId"] = order.clOrderId;
    record["market"] = to_string(order.market);
    record["securityId"] = order.securityId;
    record["side"] = to_string(order.side);
    record["price"] = order.price;
    record["qty"] = order.qty;
    record["shareholderId"] = order.shareholderId;
    enqueue(std::move(record));
}

void TradeLogger::logOrderConfirm(const std::string &clOrderId) {
    nlohmann::json record;
    record["event"] = "ORDER_CONFIRM";
    record["clOrderId"] = clOrderId;
    enqueue(std::move(record));
}

void TradeLogger::logOrderReject(const std::string &clOrderId,
                                 int32_t rejectCode,
                                 const std::string &rejectText) {
    nlohmann::json record;
    record["event"] = "ORDER_REJECT";
    record["clOrderId"] = clOrderId;
    record["rejectCode"] = rejectCode;
    record["rejectText"] = rejectText;
    enqueue(std::move(record));
}

void TradeLogger::logExecution(const std::string &execId,
                               const std::string &clOrderId,
                               const std::string &securityId, Side side,
                               uint32_t execQty, double execPrice,
                               bool isMaker) {
    nlohmann::json record;
    record["event"] = "EXECUTION";
    record["execId"] = execId;
    record["clOrderId"] = clOrderId;
    record["securityId"] = securityId;
    record["side"] = to_string(side);
    record["execQty"] = execQty;
    record["execPrice"] = execPrice;
    record["isMaker"] = isMaker;
    enqueue(std::move(record));
}

void TradeLogger::logCancelConfirm(const std::string &origClOrderId,
                                   uint32_t canceledQty, uint32_t cumQty) {
    nlohmann::json record;
    record["event"] = "CANCEL_CONFIRM";
    record["origClOrderId"] = origClOrderId;
    record["canceledQty"] = canceledQty;
    record["cumQty"] = cumQty;
    enqueue(std::move(record));
}

void TradeLogger::logCancelReject(const std::string &origClOrderId,
                                  int32_t rejectCode,
                                  const std::string &rejectText) {
    nlohmann::json record;
    record["event"] = "CANCEL_REJECT";
    record["origClOrderId"] = origClOrderId;
    record["rejectCode"] = rejectCode;
    record["rejectText"] = rejectText;
    enqueue(std::move(record));
}

void TradeLogger::logMarketData(const std::string &securityId, Market market,
                                double bidPrice, double askPrice) {
    nlohmann::json record;
    record["event"] = "MARKET_DATA";
    record["securityId"] = securityId;
    record["market"] = to_string(market);
    record["bidPrice"] = bidPrice;
    record["askPrice"] = askPrice;
    enqueue(std::move(record));
}

} // namespace hdf
