#include "security_core.h"
#include "constants.h"
#include "types.h"

// SecurityCore —— 单证券核心业务单元
// 每个 SecurityCore 拥有独立的撮合引擎、风控器和待处理状态，
// 由 TradeSystem 按 hash(market+securityId) 路由分配到对应的 WorkerBucket 中。
namespace hdf {

SecurityCore::SecurityCore() {}

SecurityCore::~SecurityCore() {}

void SecurityCore::setSendToClient(SendToClient callback) {
    sendToClient_ = callback;
}

void SecurityCore::setSendToExchange(SendToExchange callback) {
    sendToExchange_ = callback;
}

void SecurityCore::setSendMarketData(SendMarketData callback) {
    sendMarketData_ = callback;
}

void SecurityCore::setLogger(TradeLogger *logger) { logger_ = logger; }

// ============================================================
// handleOrder —— 处理新订单
// 流程: 解析JSON → 风控检查 → 撮合匹配 → 发送回报
// ============================================================
void SecurityCore::handleOrder(const nlohmann::json &input) {
    // 解析 JSON → 委托给 struct 版本
    Order order;
    try {
        order = input.get<Order>();
    } catch (const std::exception &e) {
        if (sendToClient_) {
            nlohmann::json response;
            response["clOrderId"] = input.value("clOrderId", "");
            response["rejectCode"] = ORDER_INVALID_FORMAT_REJECT_CODE;
            response["rejectText"] =
                ORDER_INVALID_FORMAT_REJECT_REASON + ": " + e.what();
            sendToClient_(response);
        }
        return;
    }
    handleOrder(std::move(order));
}

// ============================================================
// handleOrder (struct) —— 处理已解析的订单
// 流程: 风控检查 → 撮合匹配 → 发送回报
// ============================================================
void SecurityCore::handleOrder(Order order) {
    if (logger_)
        logger_->logOrderNew(order);

    // 2. 风控检查：检测自成交等违规情形
    auto riskResult = riskController_.checkOrder(order);

    // 自成交（同一股东在同一证券的买卖对冲）→ 拒绝
    if (riskResult == RiskController::RiskCheckResult::CROSS_TRADE) {
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
        if (logger_)
            logger_->logOrderReject(order.clOrderId,
                                    ORDER_CROSS_TRADE_REJECT_CODE,
                                    ORDER_CROSS_TRADE_REJECT_REASON);
    } else {
        // 风控通过，进入撮合流程
        // 纯交易所模式（无上游交易所）：直接回报确认
        if (!sendToExchange_ && sendToClient_) {
            nlohmann::json response;
            response["clOrderId"] = order.clOrderId;
            response["market"] = to_string(order.market);
            response["securityId"] = order.securityId;
            response["side"] = to_string(order.side);
            response["qty"] = order.qty;
            response["price"] = order.price;
            response["shareholderId"] = order.shareholderId;
            sendToClient_(response);
            if (logger_)
                logger_->logOrderConfirm(order.clOrderId);
        }

        // 3. 查找该证券最新行情，供撮合引擎参考
        std::optional<MarketData> marketData;
        const std::string marketKey =
            to_string(order.market) + "+" + order.securityId;
        auto marketIt = latestMarketData_.find(marketKey);
        if (marketIt != latestMarketData_.end()) {
            marketData = marketIt->second;
        }

        // 4. 撮合匹配：尝试与订单簿中的对手方订单成交
        auto matchResult = matchingEngine_.match(order, marketData);
        if (!matchResult.executions.empty()) {
            // 有成交 —— 分网关模式和交易所模式两条路径
            auto &executions = matchResult.executions;
            if (sendToExchange_) {
                // 网关模式：先向上游交易所发撤单请求，等待全部回报后再统一处理
                PendingMatch pending;
                pending.activeOrder = order;
                pending.executions = executions;
                pending.remainingQty = matchResult.remainingQty;
                pending.pendingCancelCount = executions.size();
                pendingMatches_[order.clOrderId] = std::move(pending);

                // 对每笔被动成交方发起撤单，记录映射关系
                for (const auto &exec : executions) {
                    cancelToActiveOrder_[exec.clOrderId] = order.clOrderId;

                    nlohmann::json cancelRequest;
                    cancelRequest["clOrderId"] = "";
                    cancelRequest["origClOrderId"] = exec.clOrderId;
                    cancelRequest["market"] = to_string(exec.market);
                    cancelRequest["securityId"] = exec.securityId;
                    cancelRequest["shareholderId"] = exec.shareholderId;
                    cancelRequest["side"] = to_string(exec.side);
                    sendToExchange_(cancelRequest);
                }
            } else {
                // 交易所模式：直接成交，推送成交回报给客户端
                uint32_t totalExecQty = 0;
                for (const auto &exec : executions) {
                    riskController_.onOrderExecuted(exec.clOrderId,
                                                    exec.execQty);
                    if (logger_)
                        logger_->logExecution(
                            exec.execId, exec.clOrderId, exec.securityId,
                            exec.side, exec.execQty, exec.execPrice, true);
                    totalExecQty += exec.execQty;
                    if (sendToClient_) {
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
                    if (logger_)
                        logger_->logExecution(
                            exec.execId, order.clOrderId, order.securityId,
                            order.side, exec.execQty, exec.execPrice, false);
                }
                riskController_.onOrderExecuted(order.clOrderId, totalExecQty);

                // 部分成交：将剩余数量挂入订单簿等待后续撮合
                if (matchResult.remainingQty > 0) {
                    Order remainingOrder = order;
                    remainingOrder.qty = matchResult.remainingQty;
                    matchingEngine_.addOrder(remainingOrder);
                    riskController_.onOrderAccepted(remainingOrder);
                }
            }
        } else {
            // 无成交 —— 直接挂单入簿
            if (sendToExchange_) {
                matchingEngine_.addOrder(order);
                nlohmann::json orderJson = order;
                sendToExchange_(orderJson);
            } else {
                matchingEngine_.addOrder(order);
            }
            riskController_.onOrderAccepted(order);
        }

        // 交易所模式下更新行情广播
        if (!sendToExchange_)
            broadcastMarketData(order.securityId, order.market);
    }
}

// ============================================================
// handleCancel —— 处理撤单请求
// 网关模式下区分 本地订单 和 已报交易所订单 两种路径
// ============================================================
void SecurityCore::handleCancel(const nlohmann::json &input) {
    CancelOrder order;
    try {
        order = input.get<CancelOrder>();
    } catch (const std::exception &e) {
        if (sendToClient_) {
            nlohmann::json response;
            response["clOrderId"] = input.value("clOrderId", "");
            response["origClOrderId"] = input.value("origClOrderId", "");
            response["rejectCode"] = ORDER_INVALID_FORMAT_REJECT_CODE;
            response["rejectText"] =
                ORDER_INVALID_FORMAT_REJECT_REASON + ": " + e.what();
            sendToClient_(response);
        }
        return;
    }
    handleCancel(std::move(order));
}

void SecurityCore::handleCancel(CancelOrder order) {
    if (sendToExchange_) {
        // 本地订单（撮合后留在本地簿的残单）直接本地撤销
        if (localOnlyOrders_.count(order.origClOrderId)) {
            localOnlyOrders_.erase(order.origClOrderId);
            CancelResponse result =
                matchingEngine_.cancelOrder(order.origClOrderId);
            if (result.type == CancelResponse::Type::REJECT) {
                if (logger_)
                    logger_->logCancelReject(order.origClOrderId,
                                             result.rejectCode,
                                             result.rejectText);
                if (sendToClient_) {
                    nlohmann::json response;
                    response["clOrderId"] = order.clOrderId;
                    response["origClOrderId"] = order.origClOrderId;
                    response["market"] = to_string(order.market);
                    response["securityId"] = order.securityId;
                    response["shareholderId"] = order.shareholderId;
                    response["side"] = to_string(order.side);
                    response["rejectCode"] = result.rejectCode;
                    response["rejectText"] = result.rejectText;
                    sendToClient_(response);
                }
            } else {
                riskController_.onOrderCanceled(order.origClOrderId);
                if (logger_)
                    logger_->logCancelConfirm(
                        order.origClOrderId, result.canceledQty, result.cumQty);
                if (sendToClient_) {
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
        } else {
            // 非本地订单：转发撤单请求到上游交易所
            nlohmann::json cancelJson;
            cancelJson["clOrderId"] = order.clOrderId;
            cancelJson["origClOrderId"] = order.origClOrderId;
            cancelJson["market"] = to_string(order.market);
            cancelJson["securityId"] = order.securityId;
            cancelJson["shareholderId"] = order.shareholderId;
            cancelJson["side"] = to_string(order.side);
            sendToExchange_(cancelJson);
        }
    } else {
        CancelResponse result =
            matchingEngine_.cancelOrder(order.origClOrderId);
        if (result.type == CancelResponse::Type::REJECT) {
            if (logger_)
                logger_->logCancelReject(order.origClOrderId, result.rejectCode,
                                         result.rejectText);
            if (sendToClient_) {
                nlohmann::json response;
                response["clOrderId"] = order.clOrderId;
                response["origClOrderId"] = order.origClOrderId;
                response["market"] = to_string(order.market);
                response["securityId"] = order.securityId;
                response["shareholderId"] = order.shareholderId;
                response["side"] = to_string(order.side);
                response["rejectCode"] = result.rejectCode;
                response["rejectText"] = result.rejectText;
                sendToClient_(response);
            }
        } else {
            riskController_.onOrderCanceled(order.origClOrderId);
            if (logger_)
                logger_->logCancelConfirm(order.origClOrderId,
                                          result.canceledQty, result.cumQty);
            if (sendToClient_) {
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
            broadcastMarketData(order.securityId, order.market);
        }
    }
}

// ============================================================
// handleMarketData —— 处理行情数据更新
// 缓存每个证券的最新买一/卖一价，供撮合时参考
// ============================================================
void SecurityCore::handleMarketData(const nlohmann::json &input) {
    if (!input.is_array()) {
        return;
    }

    for (const auto &item : input) {
        try {
            std::string marketStr = item.at("market").get<std::string>();
            std::string securityId = item.at("securityId").get<std::string>();
            Market market = market_from_string(marketStr);
            MarketData md;
            md.bidPrice = item.at("bidPrice").get<double>();
            md.askPrice = item.at("askPrice").get<double>();
            const std::string marketKey = to_string(market) + "+" + securityId;
            latestMarketData_[marketKey] = md;

            if (logger_)
                logger_->logMarketData(securityId, market, md.bidPrice,
                                       md.askPrice);
        } catch (const std::exception &) {
            continue;
        }
    }
}

// ============================================================
// handleResponse —— 处理上游交易所回报
// 根据回报类型分三种情况：
//   1. 含 execId      → 成交回报
//   2. 含 origClOrderId → 撤单回报（可能属于 PendingMatch 流程）
//   3. 其他           → 普通确认回报（可能触发 PendingConfirm）
// ============================================================
void SecurityCore::handleResponse(const nlohmann::json &input) {
    // —— 情况1: 成交回报 ——
    if (input.contains("execId")) {
        if (sendToClient_) {
            sendToClient_(input);
        }
        std::string clOrderId = input["clOrderId"].get<std::string>();
        uint32_t execQty = input["execQty"].get<uint32_t>();
        matchingEngine_.reduceOrderQty(clOrderId, execQty);
        riskController_.onOrderExecuted(clOrderId, execQty);
        if (logger_)
            logger_->logExecution(input.value("execId", ""), clOrderId,
                                  input.value("securityId", ""),
                                  side_from_string(input.value("side", "B")),
                                  execQty, input.value("execPrice", 0.0),
                                  false);
        // —— 情况2: 撤单回报 ——
    } else if (input.contains("origClOrderId")) {
        std::string origClOrderId = input["origClOrderId"].get<std::string>();

        // 检查是否属于 PendingMatch 流程（网关撮合后发起的撤单）
        auto reverseIt = cancelToActiveOrder_.find(origClOrderId);
        if (reverseIt != cancelToActiveOrder_.end()) {
            std::string activeOrderId = reverseIt->second;
            cancelToActiveOrder_.erase(reverseIt);

            auto it = pendingMatches_.find(activeOrderId);
            if (it == pendingMatches_.end()) {
                return;
            }
            auto &pending = it->second;

            // 统计撤单结果：成功/被拒
            if (input.contains("rejectCode")) {
                pending.rejectedIds.insert(origClOrderId);
            } else {
                pending.confirmedIds.insert(origClOrderId);
            }
            pending.pendingCancelCount--;

            // 所有撤单回报收齐后统一结算
            if (pending.pendingCancelCount == 0) {
                resolvePendingMatch(activeOrderId);
            }
        } else {
            // 普通撤单回报（非 PendingMatch 流程）
            if (input.contains("rejectCode")) {
                if (logger_)
                    logger_->logCancelReject(origClOrderId,
                                             input.value("rejectCode", 0),
                                             input.value("rejectText", ""));
                if (sendToClient_) {
                    sendToClient_(input);
                }
            } else {
                riskController_.onOrderCanceled(origClOrderId);
                matchingEngine_.cancelOrder(origClOrderId);
                if (logger_)
                    logger_->logCancelConfirm(origClOrderId,
                                              input.value("canceledQty", 0u),
                                              input.value("cumQty", 0u));
                if (sendToClient_) {
                    sendToClient_(input);
                }
            }
        }
        // —— 情况3: 普通确认回报 ——
    } else {
        std::string clOrderId = input.value("clOrderId", "");
        // 检查是否有对应的 PendingConfirm（网关撮合后重新报单的确认）
        auto confirmIt = pendingConfirms_.find(clOrderId);
        if (confirmIt != pendingConfirms_.end()) {
            auto pc = std::move(confirmIt->second);
            pendingConfirms_.erase(confirmIt);
            sendConfirmAndExecReports(pc.activeOrder, pc.confirmedExecutions);
        } else {
            if (sendToClient_) {
                sendToClient_(input);
            }
        }
    }
}

// ============================================================
// resolvePendingMatch —— 网关撮合结算
// 所有被动方撤单回报收齐后调用：
//   - 撤单成功的 → 确认成交
//   - 撤单被拒的 → 成交失败，数量归还
//   - 剩余 + 失败数量 → 重新挂单并报交易所
// ============================================================
void SecurityCore::resolvePendingMatch(const std::string &activeOrderId) {
    auto it = pendingMatches_.find(activeOrderId);
    if (it == pendingMatches_.end())
        return;
    auto &pending = it->second;

    // 统计撤单成功/失败的数量
    uint32_t rejectedQty = 0;
    uint32_t confirmedQty = 0;
    std::vector<OrderResponse> confirmedExecutions;

    for (const auto &exec : pending.executions) {
        if (pending.confirmedIds.count(exec.clOrderId)) {
            riskController_.onOrderExecuted(exec.clOrderId, exec.execQty);
            confirmedQty += exec.execQty;
            confirmedExecutions.push_back(exec);
        } else {
            rejectedQty += exec.execQty;
        }
    }

    if (confirmedQty > 0) {
        riskController_.onOrderExecuted(pending.activeOrder.clOrderId,
                                        confirmedQty);
    }

    // 未成交数量 = 撤单被拒数量 + 原始剩余数量
    uint32_t totalUnfilledQty = rejectedQty + pending.remainingQty;
    if (totalUnfilledQty > 0) {
        // 将未成交部分重新挂入本地订单簿，并向上游重新报单
        Order remainingOrder = pending.activeOrder;
        remainingOrder.qty = totalUnfilledQty;
        matchingEngine_.addOrder(remainingOrder);

        // 撤单成功的被动方如果还有残单留在本地簿，标记为本地订单
        for (const auto &exec : confirmedExecutions) {
            if (matchingEngine_.hasOrder(exec.clOrderId)) {
                localOnlyOrders_.insert(exec.clOrderId);
            }
        }

        // 保存待确认状态，等上游报单确认后再发成交回报
        PendingConfirm pc;
        pc.activeOrder = pending.activeOrder;
        pc.confirmedExecutions = std::move(confirmedExecutions);
        pendingConfirms_[activeOrderId] = std::move(pc);

        if (sendToExchange_) {
            Order remaining = pending.activeOrder;
            remaining.qty = totalUnfilledQty;
            nlohmann::json newOrder = remaining;
            sendToExchange_(newOrder);
        }
    } else {
        // 全部成交，直接发送确认和执行回报
        sendConfirmAndExecReports(pending.activeOrder, confirmedExecutions);

        for (const auto &exec : confirmedExecutions) {
            if (matchingEngine_.hasOrder(exec.clOrderId)) {
                localOnlyOrders_.insert(exec.clOrderId);
            }
        }
    }

    // 无论成交多少，主动单都标记为已接受（用于风控持仓跟踪）
    riskController_.onOrderAccepted(pending.activeOrder);

    pendingMatches_.erase(it);
}

// ============================================================
// sendConfirmAndExecReports —— 发送订单确认 + 成交回报
// 先发主动单确认，再逐笔发被动方和主动方的成交回报
// ============================================================
void SecurityCore::sendConfirmAndExecReports(
    const Order &activeOrder, const std::vector<OrderResponse> &executions) {
    if (!sendToClient_)
        return;

    if (logger_)
        logger_->logOrderConfirm(activeOrder.clOrderId);
    nlohmann::json confirm;
    confirm["clOrderId"] = activeOrder.clOrderId;
    confirm["market"] = to_string(activeOrder.market);
    confirm["securityId"] = activeOrder.securityId;
    confirm["side"] = to_string(activeOrder.side);
    confirm["qty"] = activeOrder.qty;
    confirm["price"] = activeOrder.price;
    confirm["shareholderId"] = activeOrder.shareholderId;
    sendToClient_(confirm);

    for (const auto &exec : executions) {
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

        if (logger_)
            logger_->logExecution(exec.execId, exec.clOrderId, exec.securityId,
                                  exec.side, exec.execQty, exec.execPrice,
                                  true);

        nlohmann::json activeResponse;
        activeResponse["clOrderId"] = activeOrder.clOrderId;
        activeResponse["market"] = to_string(activeOrder.market);
        activeResponse["securityId"] = activeOrder.securityId;
        activeResponse["side"] = to_string(activeOrder.side);
        activeResponse["qty"] = activeOrder.qty;
        activeResponse["price"] = activeOrder.price;
        activeResponse["shareholderId"] = activeOrder.shareholderId;
        activeResponse["execId"] = exec.execId;
        activeResponse["execQty"] = exec.execQty;
        activeResponse["execPrice"] = exec.execPrice;
        sendToClient_(activeResponse);

        if (logger_)
            logger_->logExecution(exec.execId, activeOrder.clOrderId,
                                  activeOrder.securityId, activeOrder.side,
                                  exec.execQty, exec.execPrice, false);
    }
}

// 查询完整订单簿快照
nlohmann::json SecurityCore::queryOrderbook() const {
    return matchingEngine_.getSnapshot();
}

// 查询指定证券的订单簿快照
nlohmann::json SecurityCore::queryOrderbook(const std::string &securityId,
                                            Market market) const {
    return matchingEngine_.getSnapshot(securityId, market);
}

// 广播该证券最新行情（最优买卖价）
void SecurityCore::broadcastMarketData(const std::string &securityId,
                                       Market market) {
    if (!sendMarketData_)
        return;

    MarketData md = matchingEngine_.getBestQuote(securityId, market);

    nlohmann::json data = nlohmann::json::array();
    data.push_back({{"market", to_string(market)},
                    {"securityId", securityId},
                    {"bidPrice", md.bidPrice},
                    {"askPrice", md.askPrice}});
    sendMarketData_(data);
}

} // namespace hdf
