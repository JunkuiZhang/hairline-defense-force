#pragma once

#include "security_core.h"
#include "trade_logger.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace hdf {

/** 交易指令流转流程：
 *
 * ┌──────────┐   op1:订单/撤单   ┌──────────┐   op2:订单/撤单    ┌──────────┐
 * │  用户端   │ ───────────────> │  此系统   │ ───────────────> │  交易所   │
 * └──────────┘                  └──────────┘                  └──────────┘
 *      ↑                           │   ↑                            │
 *      │                           │   │                            │
 *      └───────── op4: 回报 ────────┘   └───────── op3: 回报 ────────┘
 **/
class TradeSystem {
  public:
    TradeSystem();
    ~TradeSystem();

    using SendToClient = std::function<void(const nlohmann::json &)>;
    using SendToExchange = std::function<void(const nlohmann::json &)>;
    using SendMarketData = std::function<void(const nlohmann::json &)>;

    /**
     * @brief 启用交易历史记录。调用后所有事件将写入指定文件。
     * @param filePath JSONL 文件路径
     * @return true 打开成功
     */
    bool enableLogging(const std::string &filePath);

    /**
     * @brief 关闭交易历史记录
     */
    void disableLogging();

    /**
     * @brief 获取日志器引用（供离线分析使用）
     */
    TradeLogger &logger();

    /**
     * @brief 设置与客户端的交互接口，图中op4
     */
    void setSendToClient(SendToClient callback);
    /**
     * @brief 设置与交易所的交互接口，图中op2
     */
    void setSendToExchange(SendToExchange callback);

    /**
     * @brief 设置行情数据推送接口
     *
     * 当订单簿发生变动时，系统会自动通过此回调推送受影响证券的
     * 最新行情（best bid / best ask）。交易所前置收到后可调用
     * handleMarketData() 进行行情约束。
     */
    void setSendMarketData(SendMarketData callback);

    /**
     * @brief 处理来自客户端的订单指令，图中op1
     * @note 非线程安全，直接在调用线程中同步执行
     */
    void handleOrder(const nlohmann::json &input);
    /**
     * @brief 处理来自客户端的撤单指令，图中op1
     * @note 非线程安全，直接在调用线程中同步执行
     */
    void handleCancel(const nlohmann::json &input);
    void handleMarketData(const nlohmann::json &input);
    /**
     * @brief 处理来自交易所的回报，图中op3
     * @note 非线程安全，直接在调用线程中同步执行
     */
    void handleResponse(const nlohmann::json &input);

    /**
     * @brief 获取内部订单簿快照，返回买卖盘口价格档位。
     */
    nlohmann::json queryOrderbook() const;

    /**
     * @brief 获取指定证券的订单簿快照，路由到对应 WorkerBucket。
     */
    nlohmann::json queryOrderbook(const std::string &securityId,
                                  Market market) const;

    // ─── MPSC 命令队列接口（线程安全） ───────────────────────

    /**
     * @brief 命令类型，用于 MPSC 队列的 variant
     */
    struct CmdOrder {
        nlohmann::json input;
    };
    struct CmdCancel {
        nlohmann::json input;
    };
    struct CmdResponse {
        nlohmann::json input;
    };
    struct CmdMarketData {
        nlohmann::json input;
    };
    using Command =
        std::variant<CmdOrder, CmdCancel, CmdResponse, CmdMarketData>;

    /**
     * @brief 启动消费者线程（单线程事件循环）
     *
     * 启动后，所有通过 submitXxx() 投递的命令将在专用线程中
     * 串行执行，无需外部加锁。类似 Redis 的单线程模型。
     */
    void startEventLoop();

    /**
     * @brief 停止消费者线程，等待队列中剩余命令处理完毕
     */
    void stopEventLoop();

    /**
     * @brief 线程安全地投递订单到命令队列
     * 多个线程可同时调用，命令将在消费者线程中串行处理。
     */
    void submitOrder(const nlohmann::json &input);

    /**
     * @brief 线程安全地投递撤单到命令队列
     */
    void submitCancel(const nlohmann::json &input);

    /**
     * @brief 线程安全地投递交易所回报到命令队列
     */
    void submitResponse(const nlohmann::json &input);

    /**
     * @brief 线程安全地投递行情数据到命令队列
     */
    void submitMarketData(const nlohmann::json &input);

    /**
     * @brief 获取当前命令队列积压深度（监控用）
     */
    size_t queueDepth() const;

  private:
    // ─── WorkerBucket：每个 bucket 拥有独立的 SecurityCore + 任务队列 + 线程
    struct WorkerBucket {
        SecurityCore core;
        std::deque<Command> taskQueue;
        mutable std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> running{false};
        std::thread thread;
    };

    std::vector<std::unique_ptr<WorkerBucket>> buckets_;
    TradeLogger logger_;

    // ─── 路由 ────────────────────────────────────────────────
    static std::string makeRouteKey(const std::string &market,
                                    const std::string &securityId);
    size_t routeIndex(const std::string &market,
                      const std::string &securityId) const;
    SecurityCore &coreFor(const std::string &market,
                          const std::string &securityId);
    const SecurityCore &coreFor(const std::string &market,
                                const std::string &securityId) const;

    // ─── 每 bucket 的队列操作 ────────────────────────────────
    void enqueueToWorker(size_t idx, Command cmd);
    void workerLoop(WorkerBucket *bucket);
    static void dispatchCommand(WorkerBucket &bucket, Command &cmd);
};

} // namespace hdf
