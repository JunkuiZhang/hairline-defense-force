# 撮合引擎（Matching Engine）开发文档

## 项目概述

本文档详细说明了撮合引擎模块（模块B）的开发过程、实现细节以及单元测试设计。

**开发者**：汤语涵（[@Animnia](https://github.com/Animnia)）

---

## 一、开发内容

### 1.1 任务目标

实现一个完整的撮合引擎，支持价格优先、时间优先的撮合规则，包括订单入簿、撮合匹配、撤单、数量同步等功能。

### 1.2 核心功能

| 功能 | 说明 | 对应任务 |
|------|------|----------|
| 订单簿数据结构 | 维护买卖双边订单簿，买方价格降序+时间优先，卖方价格升序+时间优先 | B1 |
| 订单入簿 | 将订单插入订单簿对应位置，建立反向索引 | B2 |
| 基础撮合 | 价格优先、时间优先撮合，买入价≥卖出价即可成交 | B3 |
| 成交价算法 | 被动方（maker）挂单价格作为成交价 | B4 |
| 部分成交处理 | 一笔订单匹配多个对手方，逐个消耗数量 | B5 |
| 零股成交处理 | 买入单必须为100股整数倍，卖出单可以不是100股整数倍 | B6 |
| 撤单 | 从订单簿移除指定订单，返回已成交累计量 | B7 |
| 数量同步 | 减少订单簿中指定订单数量，归零则移除 | B8 |
| 成交编号生成 | 为每笔成交生成唯一的 execId | B9 |

### 1.3 数据结构设计

在 `include/matching_engine.h` 中设计了以下数据结构：

```cpp
// 订单簿中的订单条目
struct BookEntry {
    Order order;               // 原始订单信息
    uint32_t remainingQty = 0; // 剩余可成交数量
    uint32_t cumQty = 0;       // 已成交累计数量
};

// 同一价格档位上的订单队列（时间优先）
using PriceLevel = std::list<BookEntry>;

// 买方订单簿：按价格降序排列（std::greater<double>）
std::map<double, PriceLevel, std::greater<double>> bidBook_;

// 卖方订单簿：按价格升序排列（默认 std::less<double>）
std::map<double, PriceLevel> askBook_;

// 订单ID到位置的反向索引（用于快速定位）
struct OrderLocation {
    double price;
    Side side;
};
std::unordered_map<std::string, OrderLocation> orderIndex_;

// 全局成交编号计数器
uint64_t nextExecId_ = 1;
```

**设计理由**：

- **`std::map`** 按价格排序：买方使用 `std::greater` 实现价格降序（最高价优先），卖方使用默认 `std::less` 实现价格升序（最低价优先），天然满足价格优先规则
- **`std::list`** 保持插入顺序：同价格下先挂的订单排在前面，天然满足时间优先规则，且支持高效的中间删除操作
- **`orderIndex_`** 反向索引：通过订单ID直接定位到价格和方向，使 `cancelOrder` 和 `reduceOrderQty` 的查找效率为 O(1)，避免遍历整个订单簿
- **`BookEntry`** 分离原始量与剩余量：`remainingQty` 跟踪当前可交易量，`cumQty` 记录累计成交量，用于撤单回报

---

### 1.4 核心算法实现

#### addOrder() — 订单入簿（B2）

```cpp
void MatchingEngine::addOrder(const Order &order) {
    BookEntry entry;
    entry.order = order;
    entry.remainingQty = order.qty;
    entry.cumQty = 0;

    if (order.side == Side::BUY) {
        bidBook_[order.price].push_back(entry);
    } else {
        askBook_[order.price].push_back(entry);
    }

    // 建立反向索引
    orderIndex_[order.clOrderId] = {order.price, order.side};
}
```

- 根据买卖方向插入对应的订单簿
- 同时建立 `orderIndex_` 反向索引
- `push_back` 到 `list` 尾部保证时间优先

#### match() — 撮合逻辑（B3-B6）

撮合流程：

1. 根据新订单方向确定对手方订单簿（买单查卖方簿，卖单查买方簿）
2. 按价格优先遍历对手方订单簿
3. 检查价格条件是否满足（买入价 ≥ 卖出价）
4. 同价格内按时间优先（list顺序）逐个匹配
5. 每次匹配生成一笔 `OrderResponse`，包含唯一 `execId`
6. 更新对手方剩余量，完全成交则移除
7. 返回所有成交记录和主动方剩余量

**零股处理逻辑**：
- 买入单下单时已在 `from_json` 中验证为100的整数倍
- 撮合时，如果对手方（卖方）剩余量不足100股（零股），可以全部成交
- 如果对手方充足（≥100股），成交量向下取整到100的整数倍

**关键设计**：`match()` 为纯匹配操作，不会将新订单入簿。匹配到的对手方会从簿中移除或减少数量。调用方根据返回的 `remainingQty` 自行决定是否入簿。

#### cancelOrder() — 撤单（B7）

```
1. 通过 orderIndex_ 查找订单位置（O(1)）
2. 在对应的 bidBook_/askBook_ 中找到具体订单
3. 填充 CancelResponse（包含 cumQty 和 canceledQty）
4. 从订单簿和反向索引中移除
5. 如果价格档位为空，清理该档位
```

- `cumQty`：已成交累计量（部分成交后撤单时有用）
- `canceledQty`：本次撤销的数量（即剩余量）

#### reduceOrderQty() — 数量同步（B8）

用于交易所主动成交后同步内部订单簿：

```
1. 通过 orderIndex_ 快速定位
2. 更新 cumQty（累加减少量）
3. 减少 remainingQty
4. 若 remainingQty 归零，自动从订单簿和索引中移除
```

#### generateExecId() — 成交编号（B9）

格式：`"EXEC" + 16位数字（左补零）`，如 `"EXEC0000000000000001"`。

使用全局递增计数器 `nextExecId_` 保证唯一性。

---

### 1.5 修改的文件清单

| 文件 | 修改内容 |
|------|----------|
| `include/matching_engine.h` | 添加订单簿数据结构（`BookEntry`、`PriceLevel`、`bidBook_`/`askBook_`、`orderIndex_`、`generateExecId`） |
| `src/matching_engine.cpp` | 实现所有核心方法：`addOrder`、`match`、`cancelOrder`、`reduceOrderQty`、`generateExecId` |
| `tests/matching_test.cpp` | 编写完整的 32 个单元测试用例 |

---

## 二、单元测试说明

### 2.1 测试用例列表

共 32 个测试用例，覆盖所有验收标准：

| 测试分类 | 测试名称 | 验证内容 |
|----------|----------|----------|
| 基础功能 | `EmptyBookNoMatch` | 空订单簿时无法撮合 |
| 基础功能 | `ExactMatch` | 等价完全匹配：买卖同价同量 |
| 基础功能 | `SimpleMatch` | 卖单完全成交，买单部分成交 |
| 基础功能 | `PartialMatchWithRemainder` | 买单部分成交有剩余量 |
| 基础功能 | `PriceMismatchNoMatch` | 价格不匹配时不成交 |
| 价格优先 | `PricePriorityBuyMatchesLowestAsk` | 买方优先匹配最低卖价 |
| 价格优先 | `PricePrioritySellMatchesHighestBid` | 卖方优先匹配最高买价 |
| 时间优先 | `TimePrioritySamePrice` | 同价格先挂单先成交 |
| 部分成交 | `MultipleMatchesPartialFill` | 一笔订单匹配多个对手方 |
| 部分成交 | `MultiPriceLevelMatch` | 跨多个价格档位的部分成交 |
| 部分成交 | `LargeOrderPartialFillWithRemainder` | 大单部分成交有剩余 |
| 成交价 | `MakerPriceExecution` | 成交价为卖方挂单价 |
| 成交价 | `MakerPriceWhenSellerIsTaker` | 卖方主动成交时成交价为买方挂单价 |
| 零股 | `OddLotSellOrder` | 卖出零股正确成交 |
| 零股 | `MixedOddAndRoundLot` | 零股与整手混合撮合 |
| 撤单 | `CancelOrderSuccess` | 撤单成功返回完整信息 |
| 撤单 | `CancelNonexistentOrder` | 撤销不存在订单返回拒绝 |
| 撤单 | `CancelOrderThenNoMatch` | 撤单后不再参与撮合 |
| 撤单 | `CancelAfterPartialFill` | 部分成交后撤单返回正确累计量 |
| reduceQty | `ReduceOrderQtyBasic` | 减少数量后正确影响撮合 |
| reduceQty | `ReduceOrderQtyToZero` | 归零自动移除 |
| reduceQty | `ReduceNonexistentOrder` | 对不存在订单不崩溃 |
| execId | `UniqueExecIds` | 单次撮合 execId 唯一 |
| execId | `UniqueExecIdsAcrossMatches` | 跨撮合 execId 唯一 |
| 隔离 | `DifferentSecurityNoMatch` | 不同股票不互相撮合 |
| 综合 | `ComplexMultiLevelMatch` | 多档多笔综合撮合 |
| 综合 | `ConsecutiveMatchesBookState` | 连续撮合后订单簿状态正确 |
| 综合 | `AddMultipleOrdersThenMatch` | 多订单入簿后批量撮合 |
| 综合 | `CancelSellOrder` | 撤销卖方订单 |
| 综合 | `ReduceSellOrderQty` | 减少卖方订单数量 |
| 综合 | `FullyConsumedOrderRemoved` | 完全成交后正确移除 |
| 综合 | `ExecutionResponseFieldsCorrect` | 成交回报各字段正确填充 |

### 2.2 验收标准对照

| 验收标准 | 覆盖测试 |
|----------|----------|
| 等价完全匹配 | `ExactMatch` |
| 部分匹配 + 正确 remainingQty | `PartialMatchWithRemainder`, `MultipleMatchesPartialFill`, `LargeOrderPartialFillWithRemainder` |
| 价格优先 | `PricePriorityBuyMatchesLowestAsk`, `PricePrioritySellMatchesHighestBid` |
| 时间优先 | `TimePrioritySamePrice` |
| 零股正确处理 | `OddLotSellOrder`, `MixedOddAndRoundLot` |
| 撤单正确移除并返回累计量 | `CancelOrderSuccess`, `CancelAfterPartialFill`, `CancelOrderThenNoMatch` |
| reduceOrderQty 正确减少，归零移除 | `ReduceOrderQtyBasic`, `ReduceOrderQtyToZero` |

---

## 三、编译与运行

```bash
# 配置（使用 conda 环境的 cmake）
cmake -B build -S .

# 编译测试
cmake --build build --target unit_tests

# 运行全部测试
./bin/unit_tests

# 只运行撮合引擎测试
./bin/unit_tests --gtest_filter="MatchingEngineTest.*"
```
