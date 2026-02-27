# 管理界面 (Admin UI)

## 架构

```
浏览器 ──→ Streamlit (8501) ──→ FastAPI (8000) ──TCP──→ C++ AdminServer (9900)
```

| 组件 | 文件 | 语言 | 端口 | 职责 |
|------|------|------|------|------|
| **前端** | `app.py` | Python/Streamlit | 8501 | 可视化界面、下单表单 |
| **后端** | `server.py` | Python/FastAPI | 8000 | REST API + WebSocket |
| **桥接** | `bridge.py` | Python | — | TCP 连接 C++ 的异步客户端 |
| **协议** | `protocol.py` | Python | — | 消息类型定义 |
| **TCP 服务** | `admin_server.h/cpp` | C++ | 9900 | C++ 侧接收指令、推送回报 |

## 快速开始

```bash
# 1. 安装 Python 依赖
conda activate hdf
pip install -r requirements.txt

# 2. 启动（需先启动 C++ 后端）
chmod +x start.sh
./start.sh
```

启动后访问：
- **管理界面**: http://localhost:8501
- **API 文档**: http://localhost:8000/docs

## 文件说明

```
admin/
├── README.md           # 本文件
├── requirements.txt    # Python 依赖
├── start.sh            # 一键启动脚本
├── protocol.py         # TCP 协议定义（消息格式、字段）
├── bridge.py           # C++ TCP 通信桥接（asyncio）
├── server.py           # FastAPI 后端（REST + WebSocket）
└── app.py              # Streamlit 前端（管理界面）
```

C++ 侧（项目主代码中）：
```
include/admin_server.h  # TCP 服务端头文件
src/admin_server.cpp    # TCP 服务端实现（TODO）
```

## 通信协议

传输层：TCP，消息格式：**JSON Lines**（每条 JSON + `\n`）。

### Python → C++

| type | 说明 | 示例 |
|------|------|------|
| `order` | 提交订单 | `{"type":"order","clOrderId":"A001","market":"XSHG","securityId":"600030","side":"B","price":10.0,"qty":100,"shareholderId":"SH001"}` |
| `cancel` | 提交撤单 | `{"type":"cancel","clOrderId":"C001","origClOrderId":"A001","market":"XSHG","securityId":"600030","shareholderId":"SH001","side":"B"}` |
| `query` | 查询请求 | `{"type":"query","queryType":"orderbook"}` |

### C++ → Python

| type | 说明 | 示例 |
|------|------|------|
| `response` | 回报 | `{"type":"response","clOrderId":"A001","market":"XSHG",...}` |
| `snapshot` | 订单簿快照 | `{"type":"snapshot","bids":[...],"asks":[...]}` |

## 任务分配参考

| 任务 | 文件 | 难度 | 说明 |
|------|------|------|------|
| **C++ TCP 服务端** | `admin_server.cpp` | ⭐⭐⭐ | 实现 socket accept/read/write，JSON 分发 |
| **订单簿快照查询** | `admin_server.cpp` + C++ | ⭐⭐ | 在 MatchingEngine 中添加 snapshot 接口 |
| **FastAPI 后端完善** | `server.py` | ⭐⭐ | 错误处理、离线模式、回报分类 |
| **仪表盘页面** | `app.py` - 仪表盘 | ⭐ | 回报表格展示、自动刷新 |
| **成交记录页面** | `app.py` - 成交记录 | ⭐ | 过滤/搜索、表格排序 |
| **风控日志页面** | `app.py` - 风控日志 | ⭐ | 分类 tab、详情展示 |
| **下单/撤单表单** | `app.py` - 手动下单 | ⭐ | 表单已搭好，可优化交互 |

## 开发提示

- FastAPI 自带交互式 API 文档（`/docs`），可直接测试接口
- Streamlit 支持 `--reload` 热重载，修改 `app.py` 后自动刷新
- C++ 未启动时 Python 端会进入离线模式（不影响前端开发）
- `protocol.py` 中的消息结构需与 C++ 的 `handleClient()` 解析保持一致
