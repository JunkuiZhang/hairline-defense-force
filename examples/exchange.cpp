#include "trade_system.h"
#include <iostream>

int main() {
    hdf::TradeSystem system;

    system.setSendToClient([](const hdf::ClientReport &report) {
        nlohmann::json j = hdf::to_json_report(report);
        std::cout << "[Ord]: " << j.dump() << std::endl;
    });

    // 此系统是纯撮合系统，不与交易所交互，所以不设置sendToExchange_回调函数
    // system.setSendToExchange([](const hdf::ExchangeRequest &req) {
    //     nlohmann::json j = hdf::to_json_request(req);
    //     std::cout << "[Res]: " << j.dump() << std::endl;
    // });

    system.handleOrder(nlohmann::json::parse(R"({
        "clOrderId": "ORD00000000000001",
        "market": "XSHG",
        "securityId": "600030",
        "side": "B",
        "price": 10.5,
        "qty": 200,
        "shareholderId": "SH000000001"
    })"));

    return 0;
}
