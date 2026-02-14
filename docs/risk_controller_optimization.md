# [模块A] 风控引擎性能优化（扁平化索引）

## 描述
本 PR 对风控引擎的对敲检测功能进行了底层数据结构的重构与优化。通过引入“组合键 + 扁平化哈希表”的存储方案，将原本需要多层嵌套查找的逻辑优化为单次 O(1) 查找，显著提升了高并发场景下的订单检查性能，同时保持了原有业务逻辑的正确性（包括跨市场隔离、撤单处理等）。

## 主要变更

### 1. 数据结构重构 (include/risk_controller.h)
- **移除嵌套结构**：废弃了原有的多层嵌套 `unordered_map` 结构。
- **引入扁平化存储**：
    - `buySide_` / `sellSide_`：使用 `std::unordered_map<std::string, uint32_t>` 存储 `组合键 -> 该方向总挂单量`。
    - `orderMap_`：使用 `std::unordered_map<std::string, OrderInfo>` 维护订单维度的详细信息，用于撤单和成交时的快速回溯。
- **OrderInfo 增强**：结构体中增加了 `market` 和 `ShareholderId` 等字段，确保状态跟踪的完整性。

### 2. 核心算法优化 (src/risk_controller.cpp)
- **组合键生成 (`makeKey`)**：
    - 格式：`ShareholderId + "_" + Market(int) + "_" + SecurityId`。
    - 作用：将多维度的风控条件压缩为唯一的字符串键，实现 O(1) 定位。
- **对敲检测 (`isCrossTrade`)**：
    - 逻辑：根据当前订单生成 Key，直接查询**反方向** Map 中是否有存量 (`value > 0`)。
    - 复杂度：由 O(L) (L为层数) 降低为 O(1)。
- **生命周期管理**：
    - `onOrderAccepted`：累加对应 Key 的总挂单量。
    - `onOrderExecuted` / `onOrderCanceled`：根据订单 ID 反查 Key，扣减对应数量。自动清理数量归零的记录以节省内存。

### 3. 测试增强 (tests/risk_test.cpp)
- **新增测试用例**：
    - `NoCrossTradeDifferentMarket`：验证同一股东、同一代码但在不同市场（沪/深/北）交易时，不会误报对敲。
- **全量回归测试**：
    - 确保原有 13 个测试用例全部通过，覆盖部分成交、完全成交、撤单等复杂场景。

## 测试情况

### 测试结果
```text
[==========] Running 14 tests.
[  PASSED  ] 14 tests.
```

### 关键测试覆盖
| 测试用例 ID | 说明 | 状态 |
| :--- | :--- | :--- |
| `EmptyOrderBookNoCrossTrade` | 空订单簿基准测试 | ✅ 通过 |
| `CrossTradeDetectionSameShareholder` | 标准对敲场景检测 | ✅ 通过 |
| `NoCrossTradeDifferentMarket` | **(新增)** 跨市场隔离测试 | ✅ 通过 |
| `CrossTradeAfterPartialExecution` | 部分成交后剩余量仍触发对敲 | ✅ 通过 |
| `CrossTradeAfterCancel` | 撤单后对敲状态解除 | ✅ 通过 |

## 技术细节

### 对敲检测算法变迁

**Before (嵌套 Map 方案):**
```cpp
// 伪代码
Map<Shareholder, Map<Security, Map<Side, Qty>>>
查找: Find(shareholder) -> Find(security) -> Find(opposite_side)
```

**After (扁平化组合键方案):**
```cpp
Key = Shareholder + "_" + Market + "_" + Security
// 伪代码
buySide_Map<Key, BuyQty>   // 仅存储买单
sellSide_Map<Key, SellQty> // 仅存储卖单

// 查找逻辑（隐含了对 opposite_side 的检查）
if (Order.Side == BUY)  -> Check sellSide_Map.find(Key)
if (Order.Side == SELL) -> Check buySide_Map.find(Key)
```

### 性能特征
- **时间复杂度**：
    - `checkOrder`: **O(1)**
    - `onOrderAccepted`: **O(1)**
    - `onOrderExecuted/Canceled`: **O(1)**
- **空间复杂度**：
    - O(N)，N 为活跃订单数。虽然引入了字符串 Key，但消除了大量中间层 Map 的 overhead。

## 检查清单
- [x] 所有 14 个单元测试用例均 `PASSED`。
- [x] 确认 `Market` 字段已包含在组合键中，防止跨市场误报。
- [x] 确认成交/撤单逻辑中正确扣减了 `buySide_`/`sellSide_` 的计数。
- [x] 编译无警告（CMake/MinGW环境）。


