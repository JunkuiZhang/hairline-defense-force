#include "security_core.h"
#include "constants.h"
#include "types.h"

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

void SecurityCore::handleOrder(const nlohmann::json &input) {
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

    if (logger_)
        logger_->logOrderNew(order);

    auto riskResult = riskController_.checkOrder(order);

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

        std::optional<MarketData> marketData;
        const std::string marketKey =
            to_string(order.market) + "+" + order.securityId;
        auto marketIt = latestMarketData_.find(marketKey);
        if (marketIt != latestMarketData_.end()) {
            marketData = marketIt->second;
        }

        auto matchResult = matchingEngine_.match(order, marketData);
        if (!matchResult.executions.empty()) {
            auto &executions = matchResult.executions;
            if (sendToExchange_) {
                PendingMatch pending;
                pending.activeOrder = order;
                pending.activeOrderRawInput = input;
                pending.executions = executions;
                pending.remainingQty = matchResult.remainingQty;
                pending.pendingCancelCount = executions.size();
                pendingMatches_[order.clOrderId] = std::move(pending);

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

                if (matchResult.remainingQty > 0) {
                    Order remainingOrder = order;
                    remainingOrder.qty = matchResult.remainingQty;
                    matchingEngine_.addOrder(remainingOrder);
                    riskController_.onOrderAccepted(remainingOrder);
                }
            }
        } else {
            if (sendToExchange_) {
                matchingEngine_.addOrder(order);
                sendToExchange_(input);
            } else {
                matchingEngine_.addOrder(order);
            }
            riskController_.onOrderAccepted(order);
        }

        if (!sendToExchange_)
            broadcastMarketData(order.securityId, order.market);
    }
}

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

    if (sendToExchange_) {
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
            sendToExchange_(input);
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

void SecurityCore::handleResponse(const nlohmann::json &input) {
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
    } else if (input.contains("origClOrderId")) {
        std::string origClOrderId = input["origClOrderId"].get<std::string>();

        auto reverseIt = cancelToActiveOrder_.find(origClOrderId);
        if (reverseIt != cancelToActiveOrder_.end()) {
            std::string activeOrderId = reverseIt->second;
            cancelToActiveOrder_.erase(reverseIt);

            auto it = pendingMatches_.find(activeOrderId);
            if (it == pendingMatches_.end()) {
                return;
            }
            auto &pending = it->second;

            if (input.contains("rejectCode")) {
                pending.rejectedIds.insert(origClOrderId);
            } else {
                pending.confirmedIds.insert(origClOrderId);
            }
            pending.pendingCancelCount--;

            if (pending.pendingCancelCount == 0) {
                resolvePendingMatch(activeOrderId);
            }
        } else {
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
    } else {
        std::string clOrderId = input.value("clOrderId", "");
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

void SecurityCore::resolvePendingMatch(const std::string &activeOrderId) {
    auto it = pendingMatches_.find(activeOrderId);
    if (it == pendingMatches_.end())
        return;
    auto &pending = it->second;

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

    uint32_t totalUnfilledQty = rejectedQty + pending.remainingQty;
    if (totalUnfilledQty > 0) {
        Order remainingOrder = pending.activeOrder;
        remainingOrder.qty = totalUnfilledQty;
        matchingEngine_.addOrder(remainingOrder);

        for (const auto &exec : confirmedExecutions) {
            if (matchingEngine_.hasOrder(exec.clOrderId)) {
                localOnlyOrders_.insert(exec.clOrderId);
            }
        }

        PendingConfirm pc;
        pc.activeOrder = pending.activeOrder;
        pc.confirmedExecutions = std::move(confirmedExecutions);
        pendingConfirms_[activeOrderId] = std::move(pc);

        if (sendToExchange_) {
            nlohmann::json newOrder = pending.activeOrderRawInput;
            newOrder["qty"] = totalUnfilledQty;
            sendToExchange_(newOrder);
        }
    } else {
        sendConfirmAndExecReports(pending.activeOrder, confirmedExecutions);

        for (const auto &exec : confirmedExecutions) {
            if (matchingEngine_.hasOrder(exec.clOrderId)) {
                localOnlyOrders_.insert(exec.clOrderId);
            }
        }
    }

    riskController_.onOrderAccepted(pending.activeOrder);

    pendingMatches_.erase(it);
}

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

nlohmann::json SecurityCore::queryOrderbook() const {
    return matchingEngine_.getSnapshot();
}

nlohmann::json SecurityCore::queryOrderbook(const std::string &securityId,
                                            Market market) const {
    return matchingEngine_.getSnapshot(securityId, market);
}

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
