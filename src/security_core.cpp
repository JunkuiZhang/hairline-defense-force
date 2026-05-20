#include "security_core.h"
#include "constants.h"
#include "types.h"

// SecurityCore —— 单证券核心业务单元
// 每个 SecurityCore 拥有独立的撮合引擎、风控器和待处理状态，
// 由 TradeSystem 按 hash(market+securityId) 路由分配到对应的 WorkerBucket 中。
namespace hdf {

SecurityCore::SecurityCore() { latestMarketData_.set_capacity(1000); }

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
// handleOrder —— 处理新订单（JSON 入口，仅做解析）
// ============================================================
void SecurityCore::handleOrder(const nlohmann::json &input) {
    // 解析 JSON → 委托给 struct 版本
    Order order;
    try {
        order = input.get<Order>();
    } catch (const std::exception &e) {
        if (sendToClient_) {
            OrderResponse resp;
            resp.clOrderId = input.value("clOrderId", "");
            resp.rejectCode = ORDER_INVALID_FORMAT_REJECT_CODE;
            std::string msg =
                std::string(ORDER_INVALID_FORMAT_REJECT_REASON) + ": " +
                e.what();
            resp.rejectText = msg;
            resp.type = OrderResponse::REJECT;
            sendToClient_(resp);
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
    // 2. 风控检查：检测自成交等违规情形
    auto riskResult = riskController_.checkOrder(order);

    // 自成交（同一股东在同一证券的买卖对冲）→ 拒绝
    if (riskResult == RiskController::RiskCheckResult::CROSS_TRADE) {
        if (sendToClient_) {
            OrderResponse resp;
            resp.clOrderId = order.clOrderId;
            resp.market = order.market;
            resp.securityId = order.securityId;
            resp.side = order.side;
            resp.qty = order.qty;
            resp.price = order.price;
            resp.shareholderId = order.shareholderId;
            resp.rejectCode = ORDER_CROSS_TRADE_REJECT_CODE;
            resp.rejectText = ORDER_CROSS_TRADE_REJECT_REASON;
            resp.type = OrderResponse::REJECT;
            sendToClient_(resp);
        }
        if (logger_)
            logger_->logOrderReject(order.clOrderId.str(),
                                    ORDER_CROSS_TRADE_REJECT_CODE,
                                    ORDER_CROSS_TRADE_REJECT_REASON);
    } else {
        // 风控通过，进入撮合流程
        if (logger_)
            logger_->logOrderNew(order);

        // 纯交易所模式（无上游交易所）：直接回报确认
        if (!sendToExchange_ && sendToClient_) {
            OrderResponse resp;
            resp.clOrderId = order.clOrderId;
            resp.market = order.market;
            resp.securityId = order.securityId;
            resp.side = order.side;
            resp.qty = order.qty;
            resp.price = order.price;
            resp.shareholderId = order.shareholderId;
            resp.type = OrderResponse::CONFIRM;
            sendToClient_(resp);
            if (logger_)
                logger_->logOrderConfirm(order.clOrderId.str());
        }

        // 3. 查找该证券最新行情，供撮合引擎参考
        std::optional<MarketData> marketData;
        const BookKey marketKey = makeRouteKey(order.market, order.securityId);
        auto *mdPtr = latestMarketData_.get(marketKey);
        if (mdPtr) {
            marketData = *mdPtr;
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

                    CancelOrder cancelReq;
                    cancelReq.origClOrderId = exec.clOrderId;
                    cancelReq.market = exec.market;
                    cancelReq.securityId = exec.securityId;
                    cancelReq.shareholderId = exec.shareholderId;
                    cancelReq.side = exec.side;
                    sendToExchange_(cancelReq);
                }
            } else {
                // 交易所模式：直接成交，推送成交回报给客户端
                uint32_t totalExecQty = 0;
                for (const auto &exec : executions) {
                    riskController_.onOrderExecuted(exec.clOrderId,
                                                    exec.execQty);
                    if (logger_)
                        logger_->logExecution(
                            exec.execId.str(), exec.clOrderId.str(),
                            exec.securityId.str(), exec.side, exec.execQty,
                            exec.execPrice, true);
                    totalExecQty += exec.execQty;
                    if (sendToClient_) {
                        // 被动方成交回报
                        sendToClient_(exec);

                        // 主动方成交回报
                        OrderResponse activeResp;
                        activeResp.clOrderId = order.clOrderId;
                        activeResp.market = order.market;
                        activeResp.securityId = order.securityId;
                        activeResp.side = order.side;
                        activeResp.qty = order.qty;
                        activeResp.price = order.price;
                        activeResp.shareholderId = order.shareholderId;
                        activeResp.execId = exec.execId;
                        activeResp.execQty = exec.execQty;
                        activeResp.execPrice = exec.execPrice;
                        activeResp.type = OrderResponse::EXECUTION;
                        sendToClient_(activeResp);
                    }
                    if (logger_)
                        logger_->logExecution(
                            exec.execId.str(), order.clOrderId.str(),
                            order.securityId.str(), order.side, exec.execQty,
                            exec.execPrice, false);
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
                sendToExchange_(order);
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
// handleCancel —— 处理撤单请求（JSON 入口）
// ============================================================
void SecurityCore::handleCancel(const nlohmann::json &input) {
    CancelOrder order;
    try {
        order = input.get<CancelOrder>();
    } catch (const std::exception &e) {
        if (sendToClient_) {
            CancelResponse resp;
            resp.clOrderId = input.value("clOrderId", "");
            resp.origClOrderId = input.value("origClOrderId", "");
            resp.rejectCode = ORDER_INVALID_FORMAT_REJECT_CODE;
            std::string msg =
                std::string(ORDER_INVALID_FORMAT_REJECT_REASON) + ": " +
                e.what();
            resp.rejectText = msg;
            resp.type = CancelResponse::REJECT;
            sendToClient_(resp);
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
                    logger_->logCancelReject(order.origClOrderId.str(),
                                             result.rejectCode,
                                             result.rejectText.str());
                if (sendToClient_) {
                    result.clOrderId = order.clOrderId;
                    sendToClient_(result);
                }
            } else {
                riskController_.onOrderCanceled(order.origClOrderId);
                if (logger_)
                    logger_->logCancelConfirm(order.origClOrderId.str(),
                                              result.canceledQty,
                                              result.cumQty);
                if (sendToClient_) {
                    sendToClient_(result);
                }
            }
        } else {
            // 非本地订单：转发撤单请求到上游交易所
            sendToExchange_(order);
        }
    } else {
        CancelResponse result =
            matchingEngine_.cancelOrder(order.origClOrderId);
        if (result.type == CancelResponse::Type::REJECT) {
            if (logger_)
                logger_->logCancelReject(order.origClOrderId.str(),
                                         result.rejectCode,
                                         result.rejectText.str());
            if (sendToClient_) {
                result.clOrderId = order.clOrderId;
                sendToClient_(result);
            }
        } else {
            riskController_.onOrderCanceled(order.origClOrderId);
            if (logger_)
                logger_->logCancelConfirm(order.origClOrderId.str(),
                                          result.canceledQty, result.cumQty);
            if (sendToClient_) {
                sendToClient_(result);
            }
            broadcastMarketData(order.securityId, order.market);
        }
    }
}

// ============================================================
// handleMarketData —— 处理行情数据更新
// 缓存每个证券的最新买一/卖一价，供撮合时参考
// ============================================================
void SecurityCore::handleMarketData(const std::vector<MarketDataItem> &items) {
    handleMarketData(items.data(), items.size());
}

void SecurityCore::handleMarketData(const MarketDataItem *items, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const auto &item = items[i];
        MarketData md;
        md.bidPrice = item.bidPrice;
        md.askPrice = item.askPrice;
        const BookKey marketKey = makeRouteKey(item.market, item.securityId);
        latestMarketData_[marketKey] = md;

        if (logger_)
            logger_->logMarketData(item.securityId.str(), item.market,
                                   item.bidPrice, item.askPrice);
    }
}

// ============================================================
// handleResponse —— 处理上游交易所回报
// 根据回报类型分三种情况：
//   1. 含 execId      → 成交回报
//   2. 含 origClOrderId → 撤单回报（可能属于 PendingMatch 流程）
//   3. 其他           → 普通确认回报（可能触发 PendingConfirm）
// ============================================================
void SecurityCore::handleResponse(const ExchangeReport &report) {
    // —— 情况1: 成交回报 ——
    if (!report.execId.empty()) {
        if (sendToClient_) {
            OrderResponse resp;
            resp.clOrderId = report.clOrderId;
            resp.market = report.market;
            resp.securityId = report.securityId;
            resp.side = report.side;
            resp.qty = report.qty;
            resp.price = report.price;
            resp.shareholderId = report.shareholderId;
            resp.execId = report.execId;
            resp.execQty = report.execQty;
            resp.execPrice = report.execPrice;
            resp.type = OrderResponse::EXECUTION;
            sendToClient_(resp);
        }
        matchingEngine_.reduceOrderQty(report.clOrderId, report.execQty);
        riskController_.onOrderExecuted(report.clOrderId, report.execQty);
        if (logger_)
            logger_->logExecution(report.execId.str(), report.clOrderId.str(),
                                  report.securityId.str(), report.side,
                                  report.execQty, report.execPrice, false);
        // —— 情况2: 撤单回报 ——
    } else if (!report.origClOrderId.empty()) {
        OrderId origClOrderId = report.origClOrderId;

        // 检查是否属于 PendingMatch 流程（网关撮合后发起的撤单）
        auto reverseIt = cancelToActiveOrder_.find(origClOrderId);
        if (reverseIt != cancelToActiveOrder_.end()) {
            OrderId activeOrderId = reverseIt->second;
            cancelToActiveOrder_.erase(reverseIt);

            auto it = pendingMatches_.find(activeOrderId);
            if (it == pendingMatches_.end()) {
                return;
            }
            auto &pending = it->second;

            // 统计撤单结果：成功/被拒
            if (report.rejectCode != 0) {
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
            if (report.rejectCode != 0) {
                if (logger_)
                    logger_->logCancelReject(origClOrderId.str(),
                                             report.rejectCode,
                                             report.rejectText.str());
                if (sendToClient_) {
                    CancelResponse resp;
                    resp.origClOrderId = origClOrderId;
                    resp.clOrderId = report.clOrderId;
                    resp.market = report.market;
                    resp.securityId = report.securityId;
                    resp.shareholderId = report.shareholderId;
                    resp.side = report.side;
                    resp.rejectCode = report.rejectCode;
                    resp.rejectText = report.rejectText;
                    resp.type = CancelResponse::REJECT;
                    sendToClient_(resp);
                }
            } else {
                riskController_.onOrderCanceled(origClOrderId);
                matchingEngine_.cancelOrder(origClOrderId);
                if (logger_)
                    logger_->logCancelConfirm(origClOrderId.str(),
                                              report.canceledQty,
                                              report.cumQty);
                if (sendToClient_) {
                    CancelResponse resp;
                    resp.origClOrderId = origClOrderId;
                    resp.clOrderId = report.clOrderId;
                    resp.market = report.market;
                    resp.securityId = report.securityId;
                    resp.shareholderId = report.shareholderId;
                    resp.side = report.side;
                    resp.qty = report.qty;
                    resp.price = report.price;
                    resp.cumQty = report.cumQty;
                    resp.canceledQty = report.canceledQty;
                    resp.type = CancelResponse::CONFIRM;
                    sendToClient_(resp);
                }
            }
        }
        // —— 情况3: 普通确认回报 ——
    } else {
        OrderId clOrderId = report.clOrderId;
        // 检查是否有对应的 PendingConfirm（网关撮合后重新报单的确认）
        auto confirmIt = pendingConfirms_.find(clOrderId);
        if (confirmIt != pendingConfirms_.end()) {
            auto pc = std::move(confirmIt->second);
            pendingConfirms_.erase(confirmIt);
            sendConfirmAndExecReports(pc.activeOrder, pc.confirmedExecutions);
        } else {
            if (sendToClient_) {
                OrderResponse resp;
                resp.clOrderId = report.clOrderId;
                resp.market = report.market;
                resp.securityId = report.securityId;
                resp.side = report.side;
                resp.qty = report.qty;
                resp.price = report.price;
                resp.shareholderId = report.shareholderId;
                resp.type = OrderResponse::CONFIRM;
                sendToClient_(resp);
            }
        }
    }
}

// ============================================================
// resolvePendingMatch —— 网关撮合结算
// ============================================================
void SecurityCore::resolvePendingMatch(const OrderId &activeOrderId) {
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
            sendToExchange_(remaining);
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
// ============================================================
void SecurityCore::sendConfirmAndExecReports(
    const Order &activeOrder, const std::vector<OrderResponse> &executions) {
    if (!sendToClient_)
        return;

    if (logger_)
        logger_->logOrderConfirm(activeOrder.clOrderId.str());
    OrderResponse confirm;
    confirm.clOrderId = activeOrder.clOrderId;
    confirm.market = activeOrder.market;
    confirm.securityId = activeOrder.securityId;
    confirm.side = activeOrder.side;
    confirm.qty = activeOrder.qty;
    confirm.price = activeOrder.price;
    confirm.shareholderId = activeOrder.shareholderId;
    confirm.type = OrderResponse::CONFIRM;
    sendToClient_(confirm);

    for (const auto &exec : executions) {
        // 被动方回报
        sendToClient_(exec);

        if (logger_)
            logger_->logExecution(exec.execId.str(), exec.clOrderId.str(),
                                  exec.securityId.str(), exec.side,
                                  exec.execQty, exec.execPrice, true);

        // 主动方回报
        OrderResponse activeResp;
        activeResp.clOrderId = activeOrder.clOrderId;
        activeResp.market = activeOrder.market;
        activeResp.securityId = activeOrder.securityId;
        activeResp.side = activeOrder.side;
        activeResp.qty = activeOrder.qty;
        activeResp.price = activeOrder.price;
        activeResp.shareholderId = activeOrder.shareholderId;
        activeResp.execId = exec.execId;
        activeResp.execQty = exec.execQty;
        activeResp.execPrice = exec.execPrice;
        activeResp.type = OrderResponse::EXECUTION;
        sendToClient_(activeResp);

        if (logger_)
            logger_->logExecution(exec.execId.str(), activeOrder.clOrderId.str(),
                                  activeOrder.securityId.str(), activeOrder.side,
                                  exec.execQty, exec.execPrice, false);
    }
}

// 查询完整订单簿快照
nlohmann::json SecurityCore::queryOrderbook() {
    return matchingEngine_.getSnapshot();
}

// 查询指定证券的订单簿快照
nlohmann::json SecurityCore::queryOrderbook(const SecurityId &securityId,
                                            Market market) {
    return matchingEngine_.getSnapshot(securityId, market);
}

// 广播该证券最新行情（最优买卖价）
void SecurityCore::broadcastMarketData(const SecurityId &securityId,
                                       Market market) {
    if (!sendMarketData_)
        return;

    MarketData md = matchingEngine_.getBestQuote(securityId, market);

    std::vector<MarketDataItem> items;
    MarketDataItem item;
    item.market = market;
    item.securityId = securityId;
    item.bidPrice = md.bidPrice;
    item.askPrice = md.askPrice;
    items.push_back(item);
    sendMarketData_(items);
}

} // namespace hdf
