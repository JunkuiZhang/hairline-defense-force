#include "trade_logger.h"
#include <iostream>

namespace hdf {

TradeLogger::TradeLogger() {}

TradeLogger::~TradeLogger() { close(); }

// ============================================================
// 文件管理
// ============================================================

bool TradeLogger::open(const std::string &filePath) {
    // TODO: 使用 std::ofstream 以 append 模式打开文件
    filePath_ = filePath;
    isOpen_ = true;
    std::cout << "[TradeLogger] open: " << filePath << std::endl;
    return true;
}

void TradeLogger::close() {
    if (!isOpen_)
        return;
    // TODO: 关闭 ofstream
    std::cout << "[TradeLogger] close: " << filePath_ << std::endl;
    isOpen_ = false;
}

bool TradeLogger::isOpen() const { return isOpen_; }

// ============================================================
// 内部写入
// ============================================================

void TradeLogger::writeRecord(const nlohmann::json &record) {
    if (!isOpen_)
        return;
    // TODO: 添加毫秒级时间戳，序列化为单行 JSON，写入文件
    // 目前仅打印到 stdout 占位
    std::cout << "[TradeLogger] write: " << record.dump() << std::endl;
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
    writeRecord(record);
}

void TradeLogger::logOrderConfirm(const std::string &clOrderId) {
    nlohmann::json record;
    record["event"] = "ORDER_CONFIRM";
    record["clOrderId"] = clOrderId;
    writeRecord(record);
}

void TradeLogger::logOrderReject(const std::string &clOrderId,
                                 int32_t rejectCode,
                                 const std::string &rejectText) {
    nlohmann::json record;
    record["event"] = "ORDER_REJECT";
    record["clOrderId"] = clOrderId;
    record["rejectCode"] = rejectCode;
    record["rejectText"] = rejectText;
    writeRecord(record);
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
    writeRecord(record);
}

void TradeLogger::logCancelConfirm(const std::string &origClOrderId,
                                   uint32_t canceledQty, uint32_t cumQty) {
    nlohmann::json record;
    record["event"] = "CANCEL_CONFIRM";
    record["origClOrderId"] = origClOrderId;
    record["canceledQty"] = canceledQty;
    record["cumQty"] = cumQty;
    writeRecord(record);
}

void TradeLogger::logCancelReject(const std::string &origClOrderId,
                                  int32_t rejectCode,
                                  const std::string &rejectText) {
    nlohmann::json record;
    record["event"] = "CANCEL_REJECT";
    record["origClOrderId"] = origClOrderId;
    record["rejectCode"] = rejectCode;
    record["rejectText"] = rejectText;
    writeRecord(record);
}

void TradeLogger::logMarketData(const std::string &securityId, Market market,
                                double bidPrice, double askPrice) {
    nlohmann::json record;
    record["event"] = "MARKET_DATA";
    record["securityId"] = securityId;
    record["market"] = to_string(market);
    record["bidPrice"] = bidPrice;
    record["askPrice"] = askPrice;
    writeRecord(record);
}

// ============================================================
// 离线分析接口
// ============================================================

std::vector<nlohmann::json>
TradeLogger::loadFromFile(const std::string &filePath) {
    // TODO: 逐行读取 JSONL 文件，解析为 JSON 对象
    std::cout << "[TradeLogger] loadFromFile: " << filePath << std::endl;
    return {};
}

nlohmann::json
TradeLogger::analyze(const std::vector<nlohmann::json> &records) {
    // TODO: 遍历 records，按 event 类型分类汇总
    // 输出指标：
    //   - 按证券代码统计成交量、成交额、笔数
    //   - 撤单率 = CANCEL_CONFIRM 数 / ORDER_NEW 数
    //   - 拒绝分析：各 rejectCode 出现次数
    std::cout << "[TradeLogger] analyze: " << records.size() << " records"
              << std::endl;

    nlohmann::json report;
    report["totalRecords"] = records.size();
    report["summary"] = "TODO: implement analysis";
    return report;
}

} // namespace hdf
