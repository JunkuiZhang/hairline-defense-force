#include "nlohmann/json_fwd.hpp"
#include "trade_system.h"
#include <iostream>

int main() {
    hdf::TradeSystem system;

    system.setSendToClient([](const nlohmann::json &output) {
        std::cout << "[Ord]: " << output.dump() << std::endl;
    });

    // 此系统是纯撮合系统，不与交易所交互，所以不设置sendToExchange_回调函数
    // system.setSendToExchange([](const nlohmann::json &output) {
    //     std::cout << "[Res]: " << output.dump() << std::endl;
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
