# 管理界面设计文档

> 模拟股票交易对敲撮合系统 — 管理界面子系统

---

## 一、概述

管理界面为撮合系统提供可视化的 Web 管理入口，支持实时监控系统状态、手动提交订单/撤单、查看市场行情和风控日志，以及独立监控模拟交易所。

---

## 二、系统架构

```
┌───────────────────────────────┐
│      Streamlit 前端 (app.py)   │  ← 浏览器 :8501
│   仪表盘 │ 市场行情 │ 手动下单    │
│  手动撤单 │ 交易所监控 │ 风控日志 │
└──────────────┬────────────────┘
               │ REST API (HTTP :8000)
┌──────────────▼────────────────┐
│    FastAPI 后端 (server.py)    │
│   订单跟踪  │  回报分类存储      │
│  WebSocket 广播 │ 市场深度计算  │
└──────────────┬────────────────┘
               │ TCP Socket :9900 (JSON Lines)
┌──────────────▼────────────────┐
│  C++ AdminServer (TCP 9900)   │
│  ┌──────────┐  ┌────────────┐ │
│  │ gateway  │→ │  exchange  │ │
│  │ (前置系统) │←│ (模拟交易所) │ │
│  └──────────┘  └────────────┘ │
└───────────────────────────────┘
```

### 数据流

1. **用户下单**: Streamlit → `POST /api/order` → FastAPI → TCP bridge → C++ AdminServer → `gateway.handleOrder()`
2. **前置转发交易所**: gateway 内部撮合 + 风控 → `sendToExchange_` → `exchange.handleOrder()`
3. **交易所回报**: exchange 撮合 → `sendToClient_` → gateway.handleResponse → 广播(source=gateway)
4. **交易所监控回报**: exchange 回报同时以 `source="exchange"` 广播给 Python 端
5. **直接注入交易所**: Streamlit → `POST /api/order` (target=exchange) → AdminServer → `exchange.handleOrder()`

### 路由机制

AdminServer 透传消息中的 `target` 字段（默认 `"gateway"`），`admin_main.cpp` 中的回调根据 `target` 值将指令路由到 `gateway` 或 `exchange`。

---

## 三、技术选型

| 层次 | 技术 | 选型理由 |
|------|------|----------|
| 前端 | **Streamlit** | 零前端代码、纯 Python、自带表格/图表/表单组件、实时刷新 |
| 后端 | **FastAPI** | 原生异步、自带 WebSocket、Pydantic 校验、自动 API 文档 |
| 图表 | **Altair** | 声明式语法、与 Streamlit 深度集成、支持交互式图表 |
| C++↔Python | **TCP + JSON Lines** | 语言无关、调试方便 |

### 依赖

python版本要求为：`Python 3.13.12`

```
fastapi
uvicorn
streamlit
requests
pandas
altair
websockets
```

---

## 四、通信协议

### 4.1 TCP 协议（C++ ↔ Python）

- **传输层**: TCP Socket
- **消息格式**: JSON Lines（每条 JSON 对象以 `\n` 分隔）
- **编码**: UTF-8

### 4.2 消息类型

**Python → C++**:

| type | 说明 | 关键字段 |
|------|------|----------|
| `order` | 提交订单 | clOrderId, market, securityId, side, price, qty, shareholderId, target? |
| `cancel` | 提交撤单 | clOrderId, origClOrderId, market, securityId, shareholderId, side, target? |
| `query` | 查询请求 | queryType ("orderbook" / "stats") |

**C++ → Python**:

| type | 说明 | 关键字段 |
|------|------|----------|
| `response` | 回报广播 | source ("gateway" / "exchange"), 以及原始回报字段 |
| `snapshot` | 查询结果 | queryType + 查询数据 |
| `error` | 错误信息 | message |

### 4.3 回报分类规则

回报 JSON 根据字段存在性区分类型：

| 条件 | 类型 |
|------|------|
| 含 `execId` | 成交回报 |
| 含 `rejectCode` | 拒绝回报 |
| 含 `origClOrderId` 且无 `rejectCode` | 撤单确认 |
| 以上均无 | 确认回报 |

---

## 五、C++ 端实现

### 5.1 AdminServer (`include/admin_server.h` + `src/admin_server.cpp`)

TCP 服务端，负责接受 Python 后端连接、分发指令、广播回报。

| 方法 | 说明 |
|------|------|
| `start()` / `stop()` | 启动/停止服务 |
| `setOnOrder(cb)` | 设置收到订单时的回调 |
| `setOnCancel(cb)` | 设置收到撤单时的回调 |
| `setOnQuery(cb)` | 设置收到查询时的回调 |
| `broadcast(json)` | 向所有已连接客户端广播回报 |

实现要点：
- `acceptLoop()`: socket/bind/listen/accept，每个客户端一个 detached 线程
- `handleClient(fd)`: 按 `\n` 分割读取 JSON Lines，根据 `type` 分发到回调；透传 `target` 字段
- `sendToFd(fd, json)`: 完整发送，处理 EINTR，有返回值标记连接断开
- `broadcast()`: 遍历客户端 fd 列表发送，自动清理断开的连接
- TCP_NODELAY + SO_REUSEADDR

### 5.2 admin_main.cpp (`examples/admin_main.cpp`)

入口程序，组装双 TradeSystem 架构 + AdminServer。

```
gateway (前置系统)
  ├── sendToClient_   → 加 source="gateway"，broadcast 给 Python
  └── sendToExchange_ → 根据 origClOrderId 区分订单/撤单，转发 exchange

exchange (模拟交易所，纯撮合模式)
  └── sendToClient_   → 加 source="exchange"，broadcast 给 Python
                       → 同时调用 gateway.handleResponse()

AdminServer
  ├── onOrder  → 根据 target 路由到 gateway 或 exchange
  ├── onCancel → 根据 target 路由到 gateway 或 exchange
  └── onQuery  → 返回订单簿快照
```

线程安全：所有对 gateway/exchange 的操作在 `std::mutex` 保护下执行。

---

## 六、Python 后端实现

### 6.1 TcpBridge (`admin/bridge.py`)

异步 TCP 客户端，连接 C++ AdminServer。

| 方法 | 说明 |
|------|------|
| `connect()` / `disconnect()` | 建立/断开 TCP 连接 |
| `send_order(...)` | 发送订单（含 `target` 参数，默认 `"gateway"`） |
| `send_cancel(...)` | 发送撤单（含 `target` 参数） |
| `query_orderbook()` | 请求订单簿快照 |
| `on_response` | 收到 C++ 回报时的回调 |

内部实现异步接收循环（`_receive_loop`），持续解析 JSON Lines。

### 6.2 FastAPI 服务 (`admin/server.py`)

#### 数据模型

- `OrderRequest`: 含 `target` 字段（`"gateway"` / `"exchange"`），默认 `"gateway"`
- `CancelRequest`: 含 `target` 字段
- `OrderTracker`: 订单生命周期跟踪器，状态机：

```
已提交 → 已确认 → 部分成交 → 完全成交
                           ↘ 已撤单
      ↘ 已拒绝
```

#### REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/order` | 提交订单（自动生成 clOrderId，支持 target 路由） |
| POST | `/api/cancel` | 提交撤单（支持 target 路由） |
| GET | `/api/responses` | 获取 gateway 回报历史 |
| GET | `/api/orders` | 订单跟踪列表（可按状态过滤） |
| GET | `/api/orders/{id}` | 单个订单详情（含成交明细） |
| GET | `/api/market` | 市场行情（深度 + 成交记录，可按证券过滤） |
| GET | `/api/status` | gateway 系统连接状态与统计 |
| GET | `/api/exchange/responses` | 交易所侧回报历史 |
| GET | `/api/exchange/status` | 交易所侧统计信息 |
| WS | `/ws` | 实时回报推送 |

#### 回报存储

回报根据 `source` 字段分类存储：
- `source="gateway"` → `state.responses`（gateway 回报 + 订单跟踪更新）
- `source="exchange"` → `state.exchange_responses`（交易所独立回报）

#### 市场深度计算

`/api/market` 端点从活跃订单（已确认/部分成交/已提交）中实时构建买卖深度：
- 买盘：按价格降序累积
- 卖盘：按价格升序累积
- 返回最近成交记录和最新成交价

---

## 七、前端页面设计

### 7.1 页面导航

侧边栏固定导航，6 个页面：

```
📊 HDF 管理界面
├── 仪表盘
├── 市场行情
├── 手动下单
├── 手动撤单
├── 交易所监控
└── 风控日志
```

侧边栏底部显示系统连接状态（🟢已连接 / 🔴未连接）和总回报数。

### 7.2 仪表盘

| 区域 | 内容 |
|------|------|
| 顶部指标 | 成交笔数、确认回报、撤单确认、拒绝/风控（4 列 metric） |
| 下方 | 最近 20 条回报 JSON |

### 7.3 市场行情

| 区域 | 内容 |
|------|------|
| 筛选栏 | 证券代码下拉选择 + 刷新按钮 |
| 概览指标 | 最新成交价、买盘挂单量、卖盘挂单量、总成交笔数 |
| 深度图 | Altair 绘制的 Steam 风格市场深度曲线（买盘绿色/卖盘红色面积图 + 平滑曲线，最新价黄色虚线） |
| 盘口 | 左右两栏：买盘10档 / 卖盘10档（价格、数量、累积量） |
| 成交走势 | Altair 折线图（成交价格走势） |
| 成交明细 | 表格：成交编号、订单号、证券、方向、成交量、成交价、股东号 |

### 7.4 手动下单

| 区域 | 内容 |
|------|------|
| 下单表单 | 市场、证券代码、方向、价格、数量、股东号 → 通过前置系统下单 |
| 订单跟踪 | 状态筛选 + 订单卡片列表 |

**订单卡片**包含：
- 标题行：方向图标 + 证券代码 + 市场 + 订单号 + 状态标签 + 成交百分比 + ⋯ 撤单按钮（popover）
- 进度条：成交进度可视化
- 详情指标：委托价、委托量、已成交量、成交均价
- 可展开的成交明细列表

**订单状态图标**：⏳已提交 → 📩已确认 → 🔶部分成交 → ✅完全成交 / 🚫已撤单 / ❌已拒绝

### 7.5 手动撤单

| 区域 | 内容 |
|------|------|
| 撤单表单 | 原始订单号、市场、证券代码、方向、股东号 |

### 7.6 交易所监控

| 区域 | 内容 |
|------|------|
| 统计概览 | 总回报数、成交笔数、确认回报、撤单确认（4 列 metric） |
| 注入订单 | 表单：直接向交易所提交订单，模拟外部市场参与者（不经过前置风控） |
| 回报流水 | 4 个 Tab 分类展示：成交、确认、撤单、拒绝 |

**设计意图**：交易所注入功能与"手动下单"分离。手动下单固定走 gateway（经风控），注入交易所在此页面操作（模拟其他网关的订单直达交易所的场景）。

### 7.7 风控日志

| 区域 | 内容 |
|------|------|
| 分类 Tab | 对敲拦截（rejectCode=1）、格式错误（rejectCode=2）、其他 |
| 数据展示 | 每个 Tab 中展示对应拒绝回报的完整 JSON |

---

## 八、目录结构

```
admin/
├── start.sh          # 一键启动脚本（激活 conda env → uvicorn + streamlit）
├── server.py         # FastAPI 后端（REST API + WebSocket + 订单跟踪 + 回报分类存储）
├── app.py            # Streamlit 前端（6 个页面）
├── bridge.py         # 异步 TCP 桥接（连接 C++ AdminServer）
└── protocol.py       # 协议定义（消息类型枚举 + 数据类）

examples/
└── admin_main.cpp    # C++ 入口（双 TradeSystem + AdminServer + target 路由）

include/
└── admin_server.h    # AdminServer 头文件

src/
└── admin_server.cpp  # AdminServer TCP 服务端实现
```

---

## 九、启动方式

```bash
# 1. 启动 C++ 后端
cd build && ninja && cd ..
./bin/admin_main              # 监听 TCP 9900

# 2. 启动 Python 端
cd admin && bash start.sh     # 自动启动 FastAPI(:8000) + Streamlit(:8501)

# 3. 浏览器访问
open http://localhost:8501
```

---

## 十、未实现 / 待改进

| 项目 | 说明 |
|------|------|
| 仪表盘自动刷新 | 当前需手动刷新，可改为 `st_autorefresh` 定时轮询或 WebSocket 推送 |
| 交易所侧订单跟踪 | 当前仅跟踪 gateway 侧订单，交易所注入的订单未入跟踪器 |
| 前端错误处理 | 部分 API 调用失败时的用户提示可进一步完善 |
