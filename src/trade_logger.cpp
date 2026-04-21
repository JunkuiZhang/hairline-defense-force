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
        try {
            std::filesystem::create_directories(parent);
        } catch (const std::filesystem::filesystem_error &e) {
            std::cerr << "[TradeLogger] 创建目录失败: " << parent
                      << ", 错误: " << e.what() << std::endl;
            return false;
        }
    }

    file_.open(filePath, std::ios::app);
    if (!file_.is_open()) {
        std::cerr << "[TradeLogger] 无法打开文件: " << filePath << std::endl;
        return false;
    }

    // 先重置停止标记，再启动线程，避免 reopen 场景下新线程读到旧的 stop=true。
    stopRequested_ = false;
    filePath_ = filePath;
    isOpen_ = true;

    // 启动后台写入线程
    writerThread_ = std::thread(&TradeLogger::writerLoop, this);

    std::cout << "[TradeLogger] open: " << filePath << std::endl;
    return true;
}

void TradeLogger::close() {
    if (!isOpen_)
        return;

    // 先关闭对外写入口，避免 close 期间还有新日志持续入队导致 join 等待过久。
    isOpen_ = false;

    // 通知后台线程停止
    stopRequested_ = true;
    cv_.notify_one();

    // 等待后台线程写完队列剩余内容并退出
    if (writerThread_.joinable()) {
        writerThread_.join();
    }

    file_.close();
    std::cout << "[TradeLogger] close: " << filePath_ << std::endl;
}

bool TradeLogger::isOpen() const { return isOpen_; }

// ============================================================
// 异步队列
// ============================================================

void TradeLogger::enqueue(nlohmann::json record) {
    // 添加时间戳
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch())
                  .count();
    record["timestamp"] = ms;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 关闭流程中直接丢弃，避免与 close 的收尾形成竞争。
        if (stopRequested_)
            return;
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
    if (!isOpen_)
        return;

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
    if (!isOpen_)
        return;

    nlohmann::json record;
    record["event"] = "ORDER_CONFIRM";
    record["clOrderId"] = clOrderId;
    enqueue(std::move(record));
}

void TradeLogger::logOrderReject(const std::string &clOrderId,
                                 int32_t rejectCode,
                                 const std::string &rejectText) {
    if (!isOpen_)
        return;

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
                               uint32_t execQty, uint64_t execPrice,
                               bool isMaker) {
    if (!isOpen_)
        return;

    nlohmann::json record;
    record["event"] = "EXECUTION";
    record["execId"] = execId;
    record["clOrderId"] = clOrderId;
    record["securityId"] = securityId;
    record["side"] = to_string(side);
    record["execQty"] = execQty;
    record["execPrice"] = static_cast<double>(execPrice) / 10000.0;
    record["isMaker"] = isMaker;
    enqueue(std::move(record));
}

void TradeLogger::logCancelConfirm(const std::string &origClOrderId,
                                   uint32_t canceledQty, uint32_t cumQty) {
    if (!isOpen_)
        return;

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
    if (!isOpen_)
        return;

    nlohmann::json record;
    record["event"] = "CANCEL_REJECT";
    record["origClOrderId"] = origClOrderId;
    record["rejectCode"] = rejectCode;
    record["rejectText"] = rejectText;
    enqueue(std::move(record));
}

void TradeLogger::logMarketData(const std::string &securityId, Market market,
                                uint64_t bidPrice, uint64_t askPrice) {
    if (!isOpen_)
        return;

    nlohmann::json record;
    record["event"] = "MARKET_DATA";
    record["securityId"] = securityId;
    record["market"] = to_string(market);
    record["bidPrice"] = static_cast<double>(bidPrice) / 10000.0;
    record["askPrice"] = static_cast<double>(askPrice) / 10000.0;
    enqueue(std::move(record));
}

} // namespace hdf
