# TradeLogger 交易历史记录开发文档

## 项目概述

`TradeLogger` 是交易系统的历史记录组件，负责将所有交易事件以 JSONL 格式异步写入磁盘，供后续 Python 离线分析使用。

---

## 一、架构设计

```
交易主线程                          后台写入线程
┌──────────┐     enqueue()     ┌──────────────┐
│ log*()   │ ──── mutex ─────> │  writerLoop  │
│ 构建JSON │   push到queue_    │  批量取出     │
│ 立即返回  │                   │  写入文件     │
└──────────┘                   │  flush        │
                               └──────────────┘
```

- 生产者：交易线程调用 `log*()` → `enqueue()` → `mutex` + `queue_.push()`
- 消费者：后台 `writerLoop` 线程 `cv_.wait()` → `swap` 整个队列 → 批量写入
- 关闭时：`close()` 设置 `stopRequested_` → 等待队列排空 → `join` 线程 → 关闭文件


---

## 二、文件结构

| 文件 | 说明 |
|------|------|
| `include/trade_logger.h` | 类声明、接口定义 |
| `src/trade_logger.cpp` | 异步写入实现 |

---

## 三、接口说明

### 3.1 文件管理

```cpp
bool open(const std::string &filePath);  // 打开文件，启动后台线程
void close();                             // 等待队列写完，关闭文件
bool isOpen() const;
```

`open()` 会自动创建父目录（`std::filesystem::create_directories`）。

### 3.2 事件记录

所有 `log*()` 方法均为异步，不阻塞调用线程：

| 方法 | 事件类型 | 关键字段 |
|------|----------|----------|
| `logOrderNew(order)` | `ORDER_NEW` | clOrderId, market, securityId, side, price, qty, shareholderId |
| `logOrderConfirm(clOrderId)` | `ORDER_CONFIRM` | clOrderId |
| `logOrderReject(id, code, text)` | `ORDER_REJECT` | clOrderId, rejectCode, rejectText |
| `logExecution(execId, ...)` | `EXECUTION` | execId, clOrderId, securityId, side, execQty, execPrice, isMaker |
| `logCancelConfirm(origId, qty, cumQty)` | `CANCEL_CONFIRM` | origClOrderId, canceledQty, cumQty |
| `logCancelReject(origId, code, text)` | `CANCEL_REJECT` | origClOrderId, rejectCode, rejectText |
| `logMarketData(secId, market, bid, ask)` | `MARKET_DATA` | securityId, market, bidPrice, askPrice |

每条记录自动附加 `timestamp`。

---

## 四、输出格式

JSONL（JSON Lines），每行一个独立 JSON 对象：

```jsonl
{"event":"ORDER_NEW","clOrderId":"O001","market":"XSHG","securityId":"600030","side":"B","price":10.0,"qty":100,"shareholderId":"SH001","timestamp":1772524800000}
{"event":"ORDER_CONFIRM","clOrderId":"O001","timestamp":1772524800001}
{"event":"EXECUTION","execId":"EXEC0000000000000001","clOrderId":"S001","securityId":"600030","side":"S","execQty":100,"execPrice":10.0,"isMaker":true,"timestamp":1772524800002}
{"event":"EXECUTION","execId":"EXEC0000000000000001","clOrderId":"O001","securityId":"600030","side":"B","execQty":100,"execPrice":10.0,"isMaker":false,"timestamp":1772524800002}
```

Python 读取方式：
```python
import pandas as pd
df = pd.read_json("data/history.jsonl", lines=True)
```

---

## 五、TradeSystem 集成

`TradeLogger` 作为 `TradeSystem` 的成员变量 `logger_`，通过以下方法暴露：

```cpp
bool enableLogging(const std::string &filePath);  // 启用日志
void disableLogging();                              // 关闭日志
TradeLogger &logger();                              // 获取引用
```

在 `TradeSystem` 的以下事件点自动调用日志：

| 事件点 | 调用 |
|--------|------|
| 新订单解析成功 | `logOrderNew` |
| 对敲拒绝 | `logOrderReject` |
| 纯撮合确认回报 | `logOrderConfirm` |
| 内部成交（被动方） | `logExecution(isMaker=true)` |
| 内部成交（主动方） | `logExecution(isMaker=false)` |
| 交易所成交回报 | `logExecution` |
| 撤单确认 | `logCancelConfirm` |
| 撤单拒绝 | `logCancelReject` |
| 交易所撤单回报 | `logCancelConfirm / logCancelReject` |

未启用日志时（未调用 `enableLogging`），所有 `log*()` 调用会因 `isOpen_` 检查立即返回，零开销。
