# 风控引擎（对敲检测）开发文档

## 项目概述

本文档详细说明了风控引擎（对敲检测）模块的开发过程、实现细节以及如何使用 Google Test 框架编写测试程序。

---

## 一、开发内容

### 1.1 任务目标

实现一个风控引擎，用于检测和防止对敲交易（同一股东在同一股票上同时持有买卖方向的订单）。

### 1.2 核心功能

| 功能 | 说明 | 文件 |
|------|------|------|
| 对敲检测 | 检测同股东号、同股票、反方向的订单 | `src/risk_controller.cpp` |
| 订单跟踪 | 维护活跃订单的内部状态 | `include/risk_controller.h` |
| 订单生命周期管理 | 处理订单的接受、撤销、成交事件 | `src/risk_controller.cpp` |

### 1.3 数据结构设计

在 `include/risk_controller.h` 中设计了三层索引结构：

```cpp
// 订单基础信息结构
struct OrderInfo {
    std::string clOrderId;      // 客户订单ID
    std::string securityId;     // 股票代码
    Side side;                  // 买卖方向
    double price;               // 价格
    uint32_t remainingQty;      // 剩余数量
};

// 第三层：买卖方向 -> 订单列表
using SideOrders = std::unordered_map<Side, std::vector<OrderInfo>>;

// 第二层：股票代码 -> 买卖方订单映射
using SecurityOrders = std::unordered_map<std::string, SideOrders>;

// 第一层：股东号 -> 股票订单的映射（股东号 -> 股票代码 -> 买卖方订单）
using ShareholderOrders = std::unordered_map<std::string, SecurityOrders>;

// 完整的活跃订单索引（按股东号组织）
ShareholderOrders activeOrders_;
```

**设计理由**：
- 三层索引可以快速定位特定股东在特定股票上的反方向订单
- 使用 `unordered_map` 提供 O(1) 的查找效率
- 使用 `vector` 存储订单列表，便于遍历和删除

### 1.4 核心算法实现

#### isCrossTrade() - 对敲检测算法

```cpp
bool RiskController::isCrossTrade(const Order &order) {
    // 1. 查找该股东号是否存在
    auto shareholderIt = activeOrders_.find(order.shareholderId);
    if (shareholderIt == activeOrders_.end()) {
        return false;
    }

    // 2. 查找该股票是否存在
    auto securityIt = shareholderIt->second.find(order.securityId);
    if (securityIt == shareholderIt->second.end()) {
        return false;
    }

    // 3. 查找反方向订单
    Side oppositeSide = (order.side == Side::BUY) ? Side::SELL : Side::BUY;
    auto oppositeIt = securityIt->second.find(oppositeSide);
    if (oppositeIt == securityIt->second.end()) {
        return false;
    }

    // 4. 检查是否存在剩余数量大于0的订单
    for (const auto &orderInfo : oppositeIt->second) {
        if (orderInfo.remainingQty > 0) {
            return true;
        }
    }

    return false;
}
```

**对敲判断条件**：
- ✅ 相同股东号 (`shareholderId`)
- ✅ 相同股票 (`securityId`)
- ✅ 相反方向 (`side`: BUY vs SELL)
- ✅ 反方向订单有剩余数量 (`remainingQty > 0`)

#### onOrderAccepted() - 订单接受

```cpp
void RiskController::onOrderAccepted(const Order &order) {
    OrderInfo orderInfo;
    orderInfo.clOrderId = order.clOrderId;
    orderInfo.securityId = order.securityId;
    orderInfo.side = order.side;
    orderInfo.price = order.price;
    orderInfo.remainingQty = order.qty;

    // 将订单添加到三层索引中
    activeOrders_[order.shareholderId][order.securityId][order.side].push_back(orderInfo);
}
```

#### onOrderCanceled() - 订单撤销

```cpp
void RiskController::onOrderCanceled(const std::string &origClOrderId) {
    // 遍历所有活跃订单，找到并删除指定订单
    for (auto &shareholderPair : activeOrders_) {
        for (auto &securityPair : shareholderPair.second) {
            for (auto &sidePair : securityPair.second) {
                auto &orders = sidePair.second;
                for (auto it = orders.begin(); it != orders.end(); ++it) {
                    if (it->clOrderId == origClOrderId) {
                        orders.erase(it);
                        return;
                    }
                }
            }
        }
    }
}
```

#### onOrderExecuted() - 订单成交

```cpp
void RiskController::onOrderExecuted(const std::string &clOrderId, uint32_t execQty) {
    // 遍历所有活跃订单，找到并更新指定订单的剩余数量
    for (auto &shareholderPair : activeOrders_) {
        for (auto &securityPair : shareholderPair.second) {
            for (auto &sidePair : securityPair.second) {
                for (auto &orderInfo : sidePair.second) {
                    if (orderInfo.clOrderId == clOrderId) {
                        if (execQty >= orderInfo.remainingQty) {
                            orderInfo.remainingQty = 0;
                        } else {
                            orderInfo.remainingQty -= execQty;
                        }
                        return;
                    }
                }
            }
        }
    }
}
```

### 1.5 修改的文件清单

| 文件 | 修改内容 |
|------|----------|
| `include/risk_controller.h` | 添加数据结构定义 |
| `src/risk_controller.cpp` | 实现所有核心方法 |
| `tests/risk_test.cpp` | 编写完整的单元测试 |
| `CMakeLists.txt` | 修改 C++ 标准为 C++20 |
| `include/types.h` | 修复 C++20 兼容性（替换 `std::unreachable`） |

---

## 二、Google Test 测试程序编写指南

### 2.1 Google Test 框架简介

Google Test（gtest）是 Google 开源的 C++ 测试框架，提供：
- 断言宏（`EXPECT_EQ`, `ASSERT_EQ` 等）
- 测试夹具（Test Fixture）
- 测试过滤和分组
- 丰富的测试报告

### 2.2 测试文件结构

```cpp
#include "risk_controller.h"
#include "types.h"
#include <gtest/gtest.h>

using namespace hdf;

// 测试夹具：为每个测试用例提供公共的初始化和清理
class RiskControllerTest : public ::testing::Test {
  protected:
    RiskController riskController;

    // 辅助函数：创建订单对象
    Order createOrder(const std::string &clOrderId, const std::string &shareholderId,
                  const std::string &securityId, Side side, double price, uint32_t qty) {
        Order order;
        order.clOrderId = clOrderId;
        order.market = Market::XSHG;
        order.securityId = securityId;
        order.side = side;
        order.price = price;
        order.qty = qty;
        order.shareholderId = shareholderId;
        return order;
    }
};

// 测试用例
TEST_F(RiskControllerTest, TestName) {
    // 测试逻辑
    EXPECT_EQ(expected, actual);
}
```

### 2.3 测试用例编写步骤

#### 步骤 1：定义测试用例

```cpp
TEST_F(RiskControllerTest, CrossTradeDetectionSameShareholder) {
    // 准备测试数据
    Order buyOrder = createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    
    // 执行测试
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);
    
    // 添加订单到风控引擎
    riskController.onOrderAccepted(buyOrder);
    
    // 测试对敲检测
    Order sellOrder = createOrder("1002", "SH001", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);
}
```

#### 步骤 2：使用断言

| 断言类型 | 说明 | 失败后行为 |
|----------|------|------------|
| `EXPECT_EQ(expected, actual)` | 期望相等 | 继续执行 |
| `ASSERT_EQ(expected, actual)` | 断言相等 | 终止测试 |
| `EXPECT_TRUE(condition)` | 期望为真 | 继续执行 |
| `EXPECT_FALSE(condition)` | 期望为假 | 继续执行 |

#### 步骤 3：运行测试

```bash
# 编译测试
cmake --build build --target unit_tests

# 运行所有测试
./bin/unit_tests

# 运行特定测试套件
./bin/unit_tests --gtest_filter=RiskControllerTest*

# 运行特定测试用例
./bin/unit_tests --gtest_filter=RiskControllerTest.CrossTradeDetectionSameShareholder
```

### 2.4 测试用例设计原则

#### 原则 1：覆盖正常场景

```cpp
TEST_F(RiskControllerTest, EmptyOrderBookNoCrossTrade) {
    Order buyOrder = createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    EXPECT_EQ(riskController.checkOrder(buyOrder),
              RiskController::RiskCheckResult::PASSED);
}
```

#### 原则 2：覆盖边界条件

```cpp
TEST_F(RiskControllerTest, CrossTradeAfterFullExecution) {
    Order buyOrder = createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000);
    riskController.onOrderAccepted(buyOrder);

    Order sellOrder = createOrder("1002", "SH001", "600000", Side::SELL, 9.0, 500);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::CROSS_TRADE);

    // 边界：完全成交后不应再检测到对敲
    riskController.onOrderExecuted("1001", 1000);
    EXPECT_EQ(riskController.checkOrder(sellOrder),
              RiskController::RiskCheckResult::PASSED);
}
```

#### 原则 3：覆盖异常情况

```cpp
TEST_F(RiskControllerTest, CancelNonExistentOrder) {
    // 异常：撤销不存在的订单不应崩溃
    riskController.onOrderCanceled("9999");
    EXPECT_EQ(riskController.checkOrder(
                  createOrder("1001", "SH001", "600000", Side::BUY, 10.0, 1000)),
              RiskController::RiskCheckResult::PASSED);
}
```

### 2.5 完整测试用例列表

| 测试用例 | 测试目的 | 验证内容 |
|----------|----------|----------|
| `EmptyOrderBookNoCrossTrade` | 空订单簿 | 无订单时不应检测到对敲 |
| `CrossTradeDetectionSameShareholder` | 基本对敲检测 | 同股东号反方向订单应被检测 |
| `NoCrossTradeDifferentShareholder` | 不同股东号 | 不同股东号不应误报 |
| `NoCrossTradeSameSide` | 同方向 | 同方向订单不应误报 |
| `NoCrossTradeDifferentSecurity` | 不同股票 | 不同股票不应误报 |
| `CrossTradeAfterCancel` | 撤销后状态 | 撤销后对敲状态应更新 |
| `CrossTradeAfterFullExecution` | 完全成交后状态 | 完全成交后对敲状态应更新 |
| `CrossTradeAfterPartialExecution` | 部分成交后状态 | 部分成交后仍应检测到对敲 |
| `MultipleOrdersSameShareholder` | 多订单处理 | 多个订单的对敲检测 |
| `SellToBuyCrossTrade` | 反向检测 | 卖单到买单的对敲检测 |
| `MultipleShareholders` | 多股东处理 | 多个股东号的对敲检测 |
| `CancelNonExistentOrder` | 异常处理 | 撤销不存在订单的处理 |
| `ExecuteNonExistentOrder` | 异常处理 | 执行不存在订单的处理 |

### 2.6 测试结果解读

```
[==========] Running 13 tests from 1 test suite.
[----------] Global test environment set-up.
[----------] 13 tests from RiskControllerTest
[ RUN      ] RiskControllerTest.EmptyOrderBookNoCrossTrade
[       OK ] RiskControllerTest.EmptyOrderBookNoCrossTrade (0 ms)
...
[----------] 13 tests from RiskControllerTest (24 ms total)
[----------] Global test environment tear-down
[==========] 13 tests from 1 test suite ran. (28 ms total)
[  PASSED  ] 13 tests.
```

**关键信息**：
- `RUN` - 开始运行测试用例
- `OK` - 测试通过
- `PASSED` - 所有测试通过
- `24 ms total` - 测试总耗时

---

## 三、测试运行指南

### 3.1 编译测试

```bash
# 使用 CMake 构建
cmake -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++
cmake --build build --target unit_tests
```

### 3.2 运行测试

```bash
# 运行所有测试
./bin/unit_tests

# 运行风控引擎测试
./bin/unit_tests --gtest_filter=RiskControllerTest*

# 运行特定测试用例
./bin/unit_tests --gtest_filter=RiskControllerTest.CrossTradeDetectionSameShareholder

# 详细输出
./bin/unit_tests --gtest_filter=RiskControllerTest* --gtest_print_time=1
```

### 3.3 测试输出说明

| 输出 | 含义 |
|------|------|
| `[ RUN      ]` | 开始运行测试 |
| `[       OK ]` | 测试通过 |
| `[  FAILED  ]` | 测试失败 |
| `[  PASSED  ]` | 所有测试通过 |

---

## 四、总结

### 4.1 完成的工作

✅ 设计并实现了三层索引数据结构
✅ 实现了对敲检测算法
✅ 实现了订单生命周期管理
✅ 编写了 13 个完整的单元测试用例
✅ 所有测试用例通过验证

### 4.2 技术要点

- 使用 `unordered_map` 实现高效查找
- 三层索引结构支持快速对敲检测
- 订单剩余数量跟踪确保准确性
- 完整的测试覆盖所有场景

### 4.3 后续工作

- 集成到撮合引擎和交易系统
- 性能优化（如需要）
- 添加更多边界条件测试
- 实现日志记录功能

---

## 附录：相关文件链接

- [risk_controller.h](include/risk_controller.h) - 风控引擎头文件
- [risk_controller.cpp](src/risk_controller.cpp) - 风控引擎实现
- [risk_test.cpp](tests/risk_test.cpp) - 单元测试
- [types.h](include/types.h) - 数据类型定义
