#include "nlohmann/json_fwd.hpp"
#include "trade_system.h"
#include <iostream>
#include <string>

int main() {
    hdf::TradeSystem system;

    system.setOrderCallback([](const nlohmann::json &output) {
        std::cout << "[Ord]: " << output.dump() << std::endl;
    });

    system.setResponseCallback([](const nlohmann::json &output) {
        std::cout << "[Res]: " << output.dump() << std::endl;
    });

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
