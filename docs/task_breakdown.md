# 项目分工表

此文件由 AI 生成，经过了简单的校验和 AI 多轮调整，但仍可能存在不准确或不合理的地方，如遇到请及时沟通修改。

考虑到大家的水平不同，当前系统的骨架是纯**单线程**实现的，不涉及cpp的高级特性，不使用模板等复杂语法，后续开发如无特殊需求也建议保持简单易懂的风格，避免过度设计。**目前的核心目标**是实现一个功能完整、逻辑清晰、易于理解的交易系统原型。后续再根据需要逐步优化性能、增加功能、引入并发等。

## 当前代码基线

以下模块已完成，作为开发起点：

| 模块 | 文件 | 说明 |
|------|------|------|
| 数据类型 | `include/types.h` | 全部结构体定义、`Order`/`CancelOrder` 的 `from_json`、`Side`/`Market` 枚举及 `to_string`/`from_string`（非法值抛异常） |
| 常量 | `include/constants.h` | `ORDER_CROSS_TRADE_REJECT_CODE`(0x01)、`ORDER_INVALID_FORMAT_REJECT_CODE`(0x02) 及 reason 字符串 |
| 交易系统接口 | `include/trade_system.h` | 完整接口 + `PendingMatch` 异步流程 + `pendingMatches_`/`cancelToActiveOrder_` 映射 |
| 交易系统实现 | `src/trade_system.cpp` | `handleOrder`（JSON解析+风控+撮合+双方回报+前置撤单+入簿）、`handleCancel`（解析+转发/撤单）、`handleResponse`（成交转发+reduceOrderQty同步+撤单异步）、`resolvePendingMatch`（确认/拒绝分流+回报+剩余量转发入簿） |
| 撮合引擎接口 | `include/matching_engine.h` | `match`（纯匹配不入簿，Approach B）、`addOrder`、`cancelOrder`、`reduceOrderQty`、`MatchResult{executions, remainingQty}` |
| 风控引擎接口 | `include/risk_controller.h` | `checkOrder`、`isCrossTrade`、`onOrderAccepted/Canceled/Executed` |
| 示例程序 | `examples/*.cpp` | 纯撮合 + 前置模式基本示例 |

---

## 任务列表

### 模块 A — 风控引擎（对敲检测）

**难度**：⭐⭐⭐ 中等  
**预估工时**：2-3 天  
**前置依赖**：无（接口已定义）  
**适合人数**：2 人

| 编号 | 任务 | 说明 | 涉及文件 |
|------|------|------|----------|
| A1 | 设计对敲检测数据结构 | 索引活跃订单，快速判断同股东号反方向是否存在可对敲订单 | `include/risk_controller.h` |
| A2 | 实现 `isCrossTrade()` | 同股东号、同股票、反方向、则为对敲（是否需要买价≥卖价待确认） | `src/risk_controller.cpp` |
| A3 | 实现 `onOrderAccepted()` | 记录已接受订单到内部状态 | `src/risk_controller.cpp` |
| A4 | 实现 `onOrderCanceled()` | 从内部状态移除已撤订单 | `src/risk_controller.cpp` |
| A5 | 实现 `onOrderExecuted()` | 减少已成交订单剩余数量，归零则移除 | `src/risk_controller.cpp` |

**验收标准**：
- 同股东号反方向可对敲订单能被检测
- 不同股东号不误报
- 同股东号同方向不误报
- 订单撤销/成交后对敲状态正确更新
- 编写单元测试（补全 `tests/risk_test.cpp`）

---

### 模块 B — 撮合引擎

**难度**：⭐⭐⭐⭐ 较难  
**预估工时**：4-5 天  
**前置依赖**：无（接口已定义）  
**适合人数**：3 人

> **设计约定**：`match()` 为纯匹配操作，不自动入簿。调用方在 `trade_system.cpp` 中根据返回的 `remainingQty` 显式调用 `addOrder()` 入簿。

| 编号 | 任务 | 说明 | 涉及文件 |
|------|------|------|----------|
| B1 | 设计订单簿数据结构 | 维护买卖双边订单簿，买方价格降序+时间优先，卖方价格升序+时间优先 | `include/matching_engine.h` |
| B2 | 实现 `addOrder()` | 将订单插入订单簿对应位置 | `src/matching_engine.cpp` |
| B3 | 实现 `match()` — 基础撮合 | 价格优先、时间优先撮合。买入价≥卖出价即可成交。返回 `MatchResult{executions, remainingQty}` | `src/matching_engine.cpp` |
| B4 | 成交价算法 | 被动方挂单价格作为成交价（maker price） | `src/matching_engine.cpp` |
| B5 | 部分成交处理 | 一笔订单匹配多个对手方，逐个消耗数量；对手方部分成交后更新剩余量 | `src/matching_engine.cpp` |
| B6 | 零股成交处理 | 买入单必须为100股的整数倍，卖出单可以不为100股的整数倍，也就是零股 | `src/matching_engine.cpp` |
| B7 | 实现 `cancelOrder()` | 从订单簿移除指定订单，返回 `CancelResponse`（含已成交累计量） | `src/matching_engine.cpp` |
| B8 | 实现 `reduceOrderQty()` | 在订单簿中减少指定订单数量，归零则移除（交易所主动成交后同步内部簿） | `src/matching_engine.cpp` |
| B9 | execId 生成 | 为每笔成交生成唯一的 execId | `src/matching_engine.cpp` |
| B10 | （可选）行情约束撮合 | `match()` 已预留 `marketData` 参数，实现买价不高于行情卖价、卖价不低于行情买价 | `src/matching_engine.cpp` |

**验收标准**：
- 等价完全匹配：买卖同价同量，成交 1 笔
- 部分匹配：大单拆小单，多笔成交 + 正确 remainingQty
- 价格优先：优先匹配更优价格的对手方
- 时间优先：同价格先来先得
- 零股正确处理
- 撤单正确移除并返回已成交累计量
- `reduceOrderQty` 正确减少数量，归零自动移除
- 编写单元测试（补全 `tests/matching_test.cpp`）

---

### 模块 C — 系统集成测试 + 端到端示例

**难度**：⭐⭐⭐ 中等  
**预估工时**：2-3 天  
**前置依赖**：模块 A、B、C 基本完成  
**适合人数**：1 人

| 编号 | 任务 | 说明 | 涉及文件 |
|------|------|------|----------|
| C1 | 纯撮合模式集成测试 | 完整流程：下单→风控→撮合→成交回报，覆盖正常成交、对敲拒绝、部分成交 | `tests/` 新建 `trade_system_test.cpp` |
| C2 | 前置模式集成测试 | 完整流程：下单→风控→撮合→撤单请求→模拟撤单回报→最终成交回报 | `tests/trade_system_test.cpp` |
| C3 | 撤单流程测试 | handleCancel 在两种模式下的行为 | `tests/` |
| C4 | 完善 examples | 更新 `exchange.cpp` 和 `pre_exchange.cpp`，展示多订单、对敲、部分成交等场景 | `examples/` |
| C5 | JSONL 批量测试 | 基于 `demo_input.jsonl` 格式，设计完整测试数据集 | `examples/` |
| C6 | CMakeLists 更新 | 新增测试文件注册到 CMake | `CMakeLists.txt` |

**验收标准**：
- 所有集成测试通过
- 纯撮合模式和前置模式均有覆盖
- 边界场景（空订单簿、重复订单ID）有覆盖

---

### 模块 D — 高级功能（可选）

**难度**：⭐⭐⭐~⭐⭐⭐⭐ 视任务而定  
**预估工时**：各 1-3 天  
**前置依赖**：基础功能完成  
**适合人数**：1-2 人

| 编号 | 任务 | 难度 | 说明 |
|------|------|------|------|
| D1 | 行情接入 | ⭐⭐⭐ | 实现 `handleMarketData`，存储与更新行情，撮合时约束价格 |
| D2 | 撤单完整支持 | ⭐⭐ | 确保 `handleCancel` 在两种模式下完整可用 |
| D3 | 性能优化 | ⭐⭐⭐⭐ | 分析瓶颈，优化订单簿数据结构（如无锁队列），benchmark |
| D4 | 管理界面前端 | ⭐⭐⭐⭐ | Web 前端展示订单簿、成交记录、手动下单入口 |
| D5 | 交易历史存储 | ⭐⭐ | 成交/回报持久化到文件或 SQLite |
| D6 | 数据分析 | ⭐⭐⭐ | 离线分析成交历史，输出统计（成交量、价格走势等） |

---

### 模块 E — 文档与答辩

这部分我建议以后再说。

**难度**：⭐⭐ 简单  
**预估工时**：2-3 天  
**前置依赖**：所有模块  
**适合人数**：1 人（全员协助）

| 编号 | 任务 | 说明 |
|------|------|------|
| E1 | 项目设计文档 | 系统架构、模块设计、算法说明、接口定义、数据结构 |
| E2 | 答辩 PPT | 项目背景、开发过程、功能演示、技术难点、总结展望 |
| E3 | README 完善 | 编译运行说明、使用方法、项目结构说明 |


---

## 任务认领表

请在下表填写姓名认领任务：

| 任务编号 | 认领人 | 状态 | 备注 |
|----------|--------|------|------|
| 模块A |[@包一帆](https://github.com/LegendFan1104) | 正在进行 | |
| 模块B | | 未开始 | |
| 模块C | [@张峻魁](https://github.com/junkuizhang) | 未开始 | |

模块D和E我建议先别管
