#include "trade_system.h"
#include "constants.h"
#include "types.h"

namespace hdf {

TradeSystem::TradeSystem() {}

TradeSystem::~TradeSystem() {}

void TradeSystem::setSendToClient(SendToClient callback) {
    sendToClient_ = callback;
}

void TradeSystem::setSendToExchange(SendToExchange callback) {
    sendToExchange_ = callback;
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
        if (sendToClient_) {
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
            sendToClient_(response);
        }
    } else {
        // 尝试撮合交易
        auto matchResult = matchingEngine_.match(order);
        if (matchResult.has_value()) {
            auto &executions = matchResult->executions;
            if (sendToExchange_) {
                // 交易所前置模式：对手方订单之前已转发给交易所，
                // 需要先向交易所发送撤单请求，等待所有撤单确认后才发成交回报。
                PendingMatch pending;
                pending.activeOrder = order;
                pending.activeOrderRawInput = input;
                pending.executions = executions;
                pending.remainingQty = matchResult->remainingQty;
                pending.pendingCancelCount = executions.size();
                pendingMatches_[order.clOrderId] = std::move(pending);

                for (const auto &exec : executions) {
                    // 建立反向映射
                    cancelToActiveOrder_[exec.clOrderId] = order.clOrderId;

                    // 向交易所发送撤单请求
                    nlohmann::json cancelRequest;
                    // TODO: 生成撤单唯一编号
                    cancelRequest["clOrderId"] = "";
                    cancelRequest["origClOrderId"] = exec.clOrderId;
                    cancelRequest["market"] = to_string(exec.market);
                    cancelRequest["securityId"] = exec.securityId;
                    cancelRequest["shareholderId"] = exec.shareholderId;
                    cancelRequest["side"] = to_string(exec.side);
                    sendToExchange_(cancelRequest);
                }
            } else {
                // 纯撮合模式：无需等待，直接发送成交回报
                uint32_t totalExecQty = 0;
                for (const auto &exec : executions) {
                    // 更新对手方（被动方）风控状态
                    riskController_.onOrderExecuted(exec.clOrderId,
                                                    exec.execQty);
                    totalExecQty += exec.execQty;
                    if (sendToClient_) {
                        // 对手方（被动方）成交回报
                        nlohmann::json passiveResponse;
                        passiveResponse["clOrderId"] = exec.clOrderId;
                        passiveResponse["market"] = to_string(exec.market);
                        passiveResponse["securityId"] = exec.securityId;
                        passiveResponse["side"] = to_string(exec.side);
                        passiveResponse["qty"] = exec.qty;
                        passiveResponse["price"] = exec.price;
                        passiveResponse["shareholderId"] = exec.shareholderId;
                        passiveResponse["execId"] = exec.execId;
                        passiveResponse["execQty"] = exec.execQty;
                        passiveResponse["execPrice"] = exec.execPrice;
                        sendToClient_(passiveResponse);

                        // 主动方（taker）成交回报
                        nlohmann::json activeResponse;
                        activeResponse["clOrderId"] = order.clOrderId;
                        activeResponse["market"] = to_string(order.market);
                        activeResponse["securityId"] = order.securityId;
                        activeResponse["side"] = to_string(order.side);
                        activeResponse["qty"] = order.qty;
                        activeResponse["price"] = order.price;
                        activeResponse["shareholderId"] = order.shareholderId;
                        activeResponse["execId"] = exec.execId;
                        activeResponse["execQty"] = exec.execQty;
                        activeResponse["execPrice"] = exec.execPrice;
                        sendToClient_(activeResponse);
                    }
                }
                // 更新主动方风控状态
                riskController_.onOrderExecuted(order.clOrderId, totalExecQty);

                // 部分成交：剩余数量已由撮合引擎入簿，生成确认回报
                if (matchResult->remainingQty > 0 && sendToClient_) {
                    nlohmann::json confirmResponse;
                    confirmResponse["clOrderId"] = order.clOrderId;
                    confirmResponse["market"] = to_string(order.market);
                    confirmResponse["securityId"] = order.securityId;
                    confirmResponse["side"] = to_string(order.side);
                    confirmResponse["qty"] = matchResult->remainingQty;
                    confirmResponse["price"] = order.price;
                    confirmResponse["shareholderId"] = order.shareholderId;
                    sendToClient_(confirmResponse);
                }
            }
        } else {
            // 没有匹配成功：
            // 如果此系统是交易所前置，则转发给交易所；
            // 如果是纯撮合系统，则生成确认回报。
            // 添加到订单簿的操作应该在撮合引擎内部进行。
            if (sendToExchange_) {
                // 系统是交易所前置
                sendToExchange_(input);
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
                sendToClient_(response);
            }
            // 更新风控系统订单状态
            riskController_.onOrderAccepted(order);
        }
    }
}

void TradeSystem::handleCancel(const nlohmann::json &input) {
    // TODO: 解析Json输入到CancelOrder结构体，在这里同时要检测撤单格式
    // 是否正确，缺少必要字段等，若不正确则return
    CancelOrder order;

    if (sendToExchange_) {
        // 系统是交易所前置，转发给交易所
        sendToExchange_(input);
    } else {
        // 更新撮合引擎订单状态
        CancelResponse result =
            matchingEngine_.cancelOrder(order.origClOrderId);
        // 更新风控系统订单状态
        riskController_.onOrderCanceled(order.origClOrderId);
        // 纯撮合系统，生成撤单确认回报
        nlohmann::json response;
        response["clOrderId"] = result.clOrderId;
        response["origClOrderId"] = result.origClOrderId;
        response["market"] = to_string(result.market);
        response["securityId"] = result.securityId;
        response["shareholderId"] = result.shareholderId;
        response["side"] = to_string(result.side);
        response["qty"] = result.qty;
        response["price"] = result.price;
        response["cumQty"] = result.cumQty;
        response["canceledQty"] = result.canceledQty;
        sendToClient_(response);
    }
}

void TradeSystem::handleMarketData(const nlohmann::json &input) {
    // TODO:
}

void TradeSystem::handleResponse(const nlohmann::json &input) {
    if (input.contains("execId")) {
        // 处理成交回报：直接转发给客户端
        if (sendToClient_) {
            sendToClient_(input);
        }
        // TODO: 更新风控状态
        // riskController_.onOrderExecuted(...);
    } else if (input.contains("origClOrderId")) {
        // 处理撤单回报
        std::string origClOrderId = input["origClOrderId"].get<std::string>();

        // 检查是否是内部撮合触发的撤单回报
        auto reverseIt = cancelToActiveOrder_.find(origClOrderId);
        if (reverseIt != cancelToActiveOrder_.end()) {
            std::string activeOrderId = reverseIt->second;
            cancelToActiveOrder_.erase(reverseIt);

            auto it = pendingMatches_.find(activeOrderId);
            if (it == pendingMatches_.end()) {
                return; // 异常情况，不应发生
            }
            auto &pending = it->second;

            if (input.contains("rejectCode")) {
                pending.rejectedIds.insert(origClOrderId);
            } else {
                pending.confirmedIds.insert(origClOrderId);
            }
            pending.pendingCancelCount--;

            // 所有撤单回报都回来了，处理最终结果
            if (pending.pendingCancelCount == 0) {
                resolvePendingMatch(activeOrderId);
            }
        } else {
            // 普通撤单回报（用户主动撤单的确认），直接转发
            if (sendToClient_) {
                sendToClient_(input);
            }
            // TODO: 更新风控状态
            // riskController_.onOrderCanceled(origClOrderId);
        }
    } else {
        // 确认回报等，直接转发给客户端
        if (sendToClient_) {
            sendToClient_(input);
        }
    }
}

void TradeSystem::resolvePendingMatch(const std::string &activeOrderId) {
    auto it = pendingMatches_.find(activeOrderId);
    if (it == pendingMatches_.end())
        return;
    auto &pending = it->second;

    uint32_t rejectedQty = 0;

    // 对于撤单确认的部分，发送成交回报
    uint32_t confirmedQty = 0;
    for (const auto &exec : pending.executions) {
        if (pending.confirmedIds.count(exec.clOrderId)) {
            // 撤单确认 → 成交生效
            riskController_.onOrderExecuted(exec.clOrderId, exec.execQty);
            confirmedQty += exec.execQty;
            if (sendToClient_) {
                // 对手方（被动方）成交回报
                nlohmann::json passiveResponse;
                passiveResponse["clOrderId"] = exec.clOrderId;
                passiveResponse["market"] = to_string(exec.market);
                passiveResponse["securityId"] = exec.securityId;
                passiveResponse["side"] = to_string(exec.side);
                passiveResponse["qty"] = exec.qty;
                passiveResponse["price"] = exec.price;
                passiveResponse["shareholderId"] = exec.shareholderId;
                passiveResponse["execId"] = exec.execId;
                passiveResponse["execQty"] = exec.execQty;
                passiveResponse["execPrice"] = exec.execPrice;
                sendToClient_(passiveResponse);

                // 主动方（taker）成交回报
                nlohmann::json activeResponse;
                activeResponse["clOrderId"] = pending.activeOrder.clOrderId;
                activeResponse["market"] =
                    to_string(pending.activeOrder.market);
                activeResponse["securityId"] = pending.activeOrder.securityId;
                activeResponse["side"] = to_string(pending.activeOrder.side);
                activeResponse["qty"] = pending.activeOrder.qty;
                activeResponse["price"] = pending.activeOrder.price;
                activeResponse["shareholderId"] =
                    pending.activeOrder.shareholderId;
                activeResponse["execId"] = exec.execId;
                activeResponse["execQty"] = exec.execQty;
                activeResponse["execPrice"] = exec.execPrice;
                sendToClient_(activeResponse);
            }
        } else {
            // 撤单被拒 → 该部分作废，累计未成交量
            rejectedQty += exec.execQty;
        }
    }

    // 更新主动方风控状态
    if (confirmedQty > 0) {
        riskController_.onOrderExecuted(pending.activeOrder.clOrderId,
                                        confirmedQty);
    }

    // 若有作废部分或撮合时的剩余量，将未成交的量转发给交易所
    uint32_t totalUnfilledQty = rejectedQty + pending.remainingQty;
    if (totalUnfilledQty > 0 && sendToExchange_) {
        nlohmann::json newOrder = pending.activeOrderRawInput;
        newOrder["qty"] = totalUnfilledQty;
        // TODO: 可能需要生成新的 clOrderId
        sendToExchange_(newOrder);
    }

    // 主动方订单的风控状态更新
    riskController_.onOrderAccepted(pending.activeOrder);

    pendingMatches_.erase(it);
}

} // namespace hdf
