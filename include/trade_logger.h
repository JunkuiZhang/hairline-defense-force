#pragma once

#include "types.h"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <string>
#include <thread>

namespace hdf {

/**
 * @brief 交易历史记录器 — 异步记录交易系统中所有事件
 *
 * 事件类型:
 *   - ORDER_NEW:      新订单委托
 *   - ORDER_CONFIRM:  订单确认
 *   - ORDER_REJECT:   订单拒绝
 *   - EXECUTION:      成交
 *   - CANCEL_CONFIRM: 撤单确认
 *   - CANCEL_REJECT:  撤单拒绝
 *   - MARKET_DATA:    行情快照
 *
 * 存储格式: JSONL（每行一个 JSON 对象，带毫秒时间戳）
 *
 * 异步写入:
 *   log*() 方法将记录推入队列，立即返回。
 *   后台写入线程在条件变量通知下从队列中取出记录并写入文件。
 */
class TradeLogger {
  public:
    TradeLogger();
    ~TradeLogger();

    // 禁止拷贝（拥有线程和文件句柄）
    TradeLogger(const TradeLogger &) = delete;
    TradeLogger &operator=(const TradeLogger &) = delete;

    // ============================================================
    // 文件管理
    // ============================================================

    /**
     * @brief 打开日志文件并启动后台写入线程
     * @param filePath JSONL 文件路径
     * @return true 打开成功
     */
    bool open(const std::string &filePath);

    /**
     * @brief 关闭日志文件，等待队列写完后停止后台线程
     */
    void close();

    /**
     * @brief 是否已打开日志文件
     */
    bool isOpen() const;

    // ============================================================
    // 事件记录接口 — 全部异步，不阻塞调用线程
    // ============================================================

    void logOrderNew(const Order &order);
    void logOrderConfirm(const std::string &clOrderId);
    void logOrderReject(const std::string &clOrderId, int32_t rejectCode,
                        const std::string &rejectText);
    void logExecution(const std::string &execId, const std::string &clOrderId,
                      const std::string &securityId, Side side,
                      uint32_t execQty, uint64_t execPrice, bool isMaker);
    void logCancelConfirm(const std::string &origClOrderId,
                          uint32_t canceledQty, uint32_t cumQty);
    void logCancelReject(const std::string &origClOrderId, int32_t rejectCode,
                         const std::string &rejectText);
    void logMarketData(const std::string &securityId, Market market,
                       uint64_t bidPrice, uint64_t askPrice);

  private:
    /**
     * @brief 将记录推入异步队列（自动添加时间戳）
     */
    void enqueue(nlohmann::json record);

    /**
     * @brief 后台写入线程主循环
     */
    void writerLoop();

    std::string filePath_;
    std::ofstream file_;
    std::atomic<bool> isOpen_{false};

    // 异步写入队列
    std::queue<nlohmann::json> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stopRequested_{false};
    std::thread writerThread_;
};

} // namespace hdf
