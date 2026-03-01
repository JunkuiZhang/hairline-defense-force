/**
 * @brief 批量跑订单并生成交易历史记录
 *
 * 读取 data/generated_orders.jsonl（由 scripts/generate_orders.py 生成），
 * 逐行喂入 TradeSystem，TradeLogger 异步记录所有事件到 data/history.jsonl。
 *
 * 用法:
 *   python3 scripts/generate_orders.py   # 先生成订单
 *   ./bin/generate_history               # 再跑这个程序
 */
#include "trade_system.h"
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char *argv[]) {
    // 默认路径，可通过命令行参数覆盖
    std::string inputPath = "data/generated_orders.jsonl";
    std::string outputPath = "data/history.jsonl";

    if (argc >= 2)
        inputPath = argv[1];
    if (argc >= 3)
        outputPath = argv[2];

    // 打开输入文件
    std::ifstream input(inputPath);
    if (!input.is_open()) {
        std::cerr << "无法打开输入文件: " << inputPath << std::endl;
        std::cerr << "请先运行: python3 scripts/generate_orders.py"
                  << std::endl;
        return 1;
    }

    hdf::TradeSystem system;

    // 启用历史记录
    if (!system.enableLogging(outputPath)) {
        std::cerr << "无法打开日志文件: " << outputPath << std::endl;
        return 1;
    }

    // 统计计数器
    int orderCount = 0, cancelCount = 0, errorCount = 0;

    system.setSendToClient([](const nlohmann::json &output) {});

    // 逐行读取并处理
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty())
            continue;

        try {
            auto json = nlohmann::json::parse(line);

            std::string type = json.value("type", "order");
            // 移除 type 字段，该字段对 TradeSystem 无用
            json.erase("type");

            if (type == "cancel") {
                system.handleCancel(json);
                cancelCount++;
            } else {
                system.handleOrder(json);
                orderCount++;
            }
        } catch (const std::exception &e) {
            std::cerr << "解析错误: " << e.what() << " | " << line << std::endl;
            errorCount++;
        }
    }

    // 关闭日志（等待异步队列写完）
    system.disableLogging();

    std::cout << "========================================" << std::endl;
    std::cout << "交易历史生成完毕" << std::endl;
    std::cout << "  输入: " << inputPath << std::endl;
    std::cout << "  输出: " << outputPath << std::endl;
    std::cout << "  委托: " << orderCount << " 笔" << std::endl;
    std::cout << "  撤单: " << cancelCount << " 笔" << std::endl;
    if (errorCount > 0)
        std::cout << "  错误: " << errorCount << " 笔" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
