#pragma once

#include "matching_engine.h"
#include "risk_controller.h"
#include "trade_logger.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>

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
    RiskController riskController_;
    MatchingEngine matchingEngine_;
    TradeLogger logger_;

    // 以下是系统与客户端和交易所交互的接口，系统可以根据是否设置了
    // sendToExchange_来判断自己是交易所前置还是纯撮合系统。
    SendToClient sendToClient_;
    SendToExchange sendToExchange_;
    SendMarketData sendMarketData_;

    /**
     * 前置模式下内部撮合成功后，需要先向交易所发送撤单请求，
     * 等待交易所返回所有撤单确认后才能向客户端发送成交回报。
     *
     * 一个主动方订单可能匹配多个对手方订单，要等所有对手方的
     * 撤单回报都回来后，才能确定最终成交结果：
     * - 撤单确认的部分 → 成交生效，发成交回报
     * - 撤单被拒的部分 → 对手方已在交易所被他人成交，该部分作废
     * - 若有作废部分未成交的量，需重新转发给交易所
     */
    struct PendingMatch {
        Order activeOrder;                     // 主动方订单（新来的订单）
        nlohmann::json activeOrderRawInput;    // 主动方原始JSON（转发用）
        std::vector<OrderResponse> executions; // 本次撮合产生的所有成交
        uint32_t remainingQty = 0;             // 撮合后未成交的剩余数量
        size_t pendingCancelCount = 0;         // 还在等待多少个撤单回报
        std::unordered_set<std::string>
            confirmedIds;                            // 已确认撤回的对手方订单ID
        std::unordered_set<std::string> rejectedIds; // 撤单被拒的对手方订单ID
    };

    // key: 主动方订单的 clOrderId
    std::unordered_map<std::string, PendingMatch> pendingMatches_;
    // 反向映射: 对手方订单ID → 主动方订单ID，用于收到撤单回报时查找归属
    std::unordered_map<std::string, std::string> cancelToActiveOrder_;

    /**
     * 前置模式下，部分内部成交后剩余量转发给交易所时，
     * 需要等待交易所的确认回报后再向客户端发送确认回报和成交回报。
     */
    struct PendingConfirm {
        Order activeOrder;                              // 主动方原始订单
        std::vector<OrderResponse> confirmedExecutions; // 已确认的内部成交
    };

    // key: 主动方订单的 clOrderId
    std::unordered_map<std::string, PendingConfirm> pendingConfirms_;

    /**
     * 记录仅存在于内部订单簿而不在交易所的订单ID。
     * 场景：内部撮合部分成交对手方后，交易所已全量撤单，
     * 但对手方仍有剩余在内部簿。此时用户撤单应走本地路径。
     */
    std::unordered_set<std::string> localOnlyOrders_;

    /**
     * 记录不同市场不同证券的最新行情数据，用于更复杂的撮合逻辑
     *
     * key: market + securityId 的组合键，如：`XSHG+600030`
     * value: 最新的 MarketData 结构体
     */
    std::unordered_map<std::string, MarketData> latestMarketData_;

    /**
     * @brief 所有撤单回报都回来后，处理最终结果
     */
    void resolvePendingMatch(const std::string &activeOrderId);

    /**
     * @brief 向客户端发送确认回报和成交回报
     */
    void
    sendConfirmAndExecReports(const Order &activeOrder,
                              const std::vector<OrderResponse> &executions);

    /**
     * @brief 推送指定证券的最新行情数据
     *
     * 从内部订单簿获取 best bid/ask，通过 sendMarketData_ 回调发送。
     * 仅在设置了 sendMarketData_ 回调时生效。
     * 且仅在当前系统为交易所，即纯撮合系统时调用。
     * 如果是交易所前置模式，则由交易所负责推送行情数据。
     */
    void broadcastMarketData(const std::string &securityId, Market market);

    // ─── MPSC 命令队列内部成员 ──────────────────────────────
    mutable std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<Command> commandQueue_;
    std::atomic<bool> eventLoopRunning_{false};
    std::thread eventLoopThread_;

    /**
     * @brief 向命令队列投递一条命令（内部通用方法）
     */
    void enqueueCommand(Command cmd);

    /**
     * @brief 消费者线程主循环
     */
    void eventLoop();

    /**
     * @brief 分发并执行一条命令
     */
    void dispatchCommand(Command &cmd);
};

} // namespace hdf
