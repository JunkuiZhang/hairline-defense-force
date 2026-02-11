#include "trade_system.h"
#include "constants.h"
#include "types.h"

namespace hdf {

TradeSystem::TradeSystem() {}

TradeSystem::~TradeSystem() {}

void TradeSystem::setOrderCallback(OrderCallback callback) {
    orderCallback_ = callback;
}

void TradeSystem::setResponseCallback(ResponseCallback callback) {
    responseCallback_ = callback;
}

void TradeSystem::handleOrder(const nlohmann::json &input) {
    Order order;
    // TODO: 解析Json输入到Order结构体
    // 在这里同时要检测订单格式是否正确，缺少必要字段等，若不正确则直接输出Reject
    // Report order.clOrderId = input["clOrderId"].get<std::string>();
    // ...

    // 风控
    auto riskResult = riskController_.checkOrder(order);

    if (riskResult == RiskController::RiskCheckResult::CROSS_TRADE) {
        // 检测到对敲，生成对敲非法回报，并传给客户端
        if (orderCallback_) {
            nlohmann::json response;
            response["clOrderId"] = order.clOrderId;
            response["market"] = to_string(order.market);
            response["securityId"] = order.securityId;
            response["side"] = to_string(order.side);
            response["qty"] = order.qty;
            response["price"] = order.price;
            response["shareholderId"] = order.shareholderId;
            response["rejectCode"] = ORDER_CROSS_TRADE_REJECT_CODE;
            response["rejectText"] = ORDER_CROSS_TRADE_REJECT_REASON;
            orderCallback_(response);
        }
    } else {
        // 尝试撮合交易
        auto matchResult = matchingEngine_.match(order);
        if (matchResult.has_value()) {
            auto &executions = matchResult->executions;
            for (const auto &exec : executions) {
                // 成交的对手单要从交易所撤回
                matchingEngine_.cancelOrder(exec.clOrderId);
                // 成交的对手单要在风控系统中更新状态
                riskController_.onOrderExecuted(exec.clOrderId, exec.execQty);
                // 生成成交回报并传给客户端
                if (orderCallback_) {
                    nlohmann::json response;
                    response["clOrderId"] = exec.clOrderId;
                    response["market"] = to_string(exec.market);
                    response["securityId"] = exec.securityId;
                    response["side"] = to_string(exec.side);
                    response["qty"] = exec.qty;
                    response["price"] = exec.price;
                    response["shareholderId"] = exec.shareholderId;
                    response["execId"] = exec.execId;
                    response["execQty"] = exec.execQty;
                    response["execPrice"] = exec.execPrice;
                    orderCallback_(response);
                }
            }
        } else {
            // 没有匹配成功：
            // 如果此系统是交易所前置，则转发给交易所；
            // 如果是纯撮合系统，则生成确认回报。
            // 添加到订单簿的操作应该在撮合引擎内部进行。
            if (responseCallback_) {
                // 系统是交易所前置
                responseCallback_(input);
            } else {
                // 纯撮合系统，生成确认回报
                nlohmann::json response;
                response["clOrderId"] = order.clOrderId;
                response["market"] = to_string(order.market);
                response["securityId"] = order.securityId;
                response["side"] = to_string(order.side);
                response["qty"] = order.qty;
                response["price"] = order.price;
                response["shareholderId"] = order.shareholderId;
                orderCallback_(response);
            }
            // 更新风控系统订单状态
            riskController_.onOrderAccepted(order);
        }
    }
}

void TradeSystem::handleCancel(const nlohmann::json &input) {
    CancelOrder order;
    // TODO: 解析Json输入到CancelOrder结构体，在这里同时要检测撤单格式
    // 是否正确，缺少必要字段等，若不正确则return
    if (responseCallback_) {
        // 系统是交易所前置，转发给交易所
        responseCallback_(input);
    } else {
        // 更新风控系统订单状态
        riskController_.onOrderCanceled(order.origClOrderId);
        // 纯撮合系统，生成撤单确认回报
        nlohmann::json response;
        response["clOrderId"] = order.clOrderId;
        response["origClOrderId"] = order.origClOrderId;
        response["market"] = to_string(order.market);
        response["securityId"] = order.securityId;
        response["shareholderId"] = order.shareholderId;
        response["side"] = to_string(order.side);
        // response["qty"] = order.qty;
        // response["price"] = order.price;
        // response["cumQty"] = order.cumQty;
        // response["canceledQty"] = order.canceledQty;
        orderCallback_(response);
    }
}

void TradeSystem::handleMarketData(const nlohmann::json &input) {
    // TODO:
}

} // namespace hdf
