#pragma once

#include "types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace hdf {

/**
 * @brief 交易历史记录器 — 记录交易系统中所有事件，并支持离线分析
 *
 * 事件类型:
 *   - ORDER_NEW:    新订单委托
 *   - ORDER_CONFIRM:订单确认
 *   - EXECUTION:    成交
 *   - CANCEL:       撤单（确认/拒绝）
 *   - REJECT:       订单拒绝
 *   - MARKET_DATA:  行情快照
 *
 * 存储格式: JSONL（每行一个 JSON 对象，带毫秒时间戳）
 *
 * 使用方式:
 *   TradeLogger logger;
 *   logger.open("data/history_20260301.jsonl");
 *   logger.logOrderNew(order);
 *   ...
 *   logger.close();
 *
 * 离线分析:
 *   auto records = TradeLogger::loadFromFile("data/history_20260301.jsonl");
 *   auto report  = TradeLogger::analyze(records);
 */
class TradeLogger {
  public:
    TradeLogger();
    ~TradeLogger();

    // ============================================================
    // 文件管理
    // ============================================================

    /**
     * @brief 打开日志文件（append 模式）
     * @param filePath JSONL 文件路径，如 "data/history_20260301.jsonl"
     * @return true 打开成功
     */
    bool open(const std::string &filePath);

    /**
     * @brief 关闭日志文件
     */
    void close();

    /**
     * @brief 是否已打开日志文件
     */
    bool isOpen() const;

    // ============================================================
    // 事件记录接口 — 由 TradeSystem 在各事件点调用
    // ============================================================

    /**
     * @brief 记录新订单委托
     */
    void logOrderNew(const Order &order);

    /**
     * @brief 记录订单确认
     */
    void logOrderConfirm(const std::string &clOrderId);

    /**
     * @brief 记录订单拒绝
     */
    void logOrderReject(const std::string &clOrderId, int32_t rejectCode,
                        const std::string &rejectText);

    /**
     * @brief 记录成交事件
     * @param execId     成交编号
     * @param clOrderId  关联订单号
     * @param securityId 证券代码
     * @param side       买卖方向
     * @param execQty    成交数量
     * @param execPrice  成交价格
     * @param isMaker    是否为被动方
     */
    void logExecution(const std::string &execId, const std::string &clOrderId,
                      const std::string &securityId, Side side,
                      uint32_t execQty, double execPrice, bool isMaker);

    /**
     * @brief 记录撤单确认
     */
    void logCancelConfirm(const std::string &origClOrderId,
                          uint32_t canceledQty, uint32_t cumQty);

    /**
     * @brief 记录撤单拒绝
     */
    void logCancelReject(const std::string &origClOrderId, int32_t rejectCode,
                         const std::string &rejectText);

    /**
     * @brief 记录行情快照
     */
    void logMarketData(const std::string &securityId, Market market,
                       double bidPrice, double askPrice);

    // ============================================================
    // 离线分析接口
    // ============================================================

    /**
     * @brief 从 JSONL 文件加载所有历史记录
     * @param filePath 文件路径
     * @return JSON 数组，每个元素为一条记录
     */
    static std::vector<nlohmann::json>
    loadFromFile(const std::string &filePath);

    /**
     * @brief 对历史记录进行分析，输出分析报告
     *
     * 分析维度：
     *   - 成交汇总：按股票统计成交量、成交额、笔数
     *   - 撤单率：撤单笔数 / 总委托笔数
     *   - 拒绝分析：各类拒绝原因占比
     *
     * @param records loadFromFile 返回的记录
     * @return JSON 格式的分析报告
     */
    static nlohmann::json analyze(const std::vector<nlohmann::json> &records);

  private:
    /**
     * @brief 写入一条记录（自动添加时间戳）
     */
    void writeRecord(const nlohmann::json &record);

    std::string filePath_;
    bool isOpen_ = false;
};

} // namespace hdf
