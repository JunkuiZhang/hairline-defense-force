"""
server.py — FastAPI 后端服务

职责：
  - 提供 REST API 供前端提交订单/撤单
  - 提供 WebSocket 端点供前端实时接收回报
  - 通过 TcpBridge 与 C++ TradeSystem 通信

启动方式：
  conda activate hdf
  uvicorn server:app --host 0.0.0.0 --port 8000 --reload

TODO: 由组员完善以下标记为 TODO 的部分
"""

import asyncio
import json
import logging
import time
import uuid
from contextlib import asynccontextmanager
from typing import Optional

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

from bridge import TcpBridge

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


# ======================== 数据模型 ========================


class OrderRequest(BaseModel):
    """下单请求"""

    market: str = Field(..., pattern="^(XSHG|XSHE|BJSE)$", description="市场")
    securityId: str = Field(..., min_length=1, description="证券代码")
    side: str = Field(..., pattern="^(B|S)$", description="买卖方向")
    price: float = Field(..., gt=0, description="价格")
    qty: int = Field(..., gt=0, description="数量")
    shareholderId: str = Field(..., min_length=1, description="股东号")
    target: str = Field("gateway", pattern="^(gateway|exchange)$", description="目标系统")


class CancelRequest(BaseModel):
    """撤单请求"""

    origClOrderId: str = Field(..., min_length=1, description="原始订单号")
    market: str = Field(..., pattern="^(XSHG|XSHE|BJSE)$")
    securityId: str = Field(..., min_length=1)
    shareholderId: str = Field(..., min_length=1)
    side: str = Field(..., pattern="^(B|S)$")
    target: str = Field("gateway", pattern="^(gateway|exchange)$", description="目标系统")


# ======================== 全局状态 ========================


class OrderTracker:
    """单个订单的跟踪状态"""

    def __init__(self, cl_order_id: str, market: str, security_id: str,
                 side: str, price: float, qty: int, shareholder_id: str):
        self.cl_order_id = cl_order_id
        self.market = market
        self.security_id = security_id
        self.side = side
        self.price = price
        self.qty = qty  # 原始委托数量
        self.shareholder_id = shareholder_id
        self.filled_qty = 0  # 已成交数量
        self.avg_price = 0.0  # 成交均价
        self.status = "已提交"  # 已提交 / 已确认 / 部分成交 / 完全成交 / 已拒绝 / 已撤单
        self.fills: list[dict] = []  # 成交明细 [{execId, execQty, execPrice}]
        self.reject_reason = ""  # 拒绝原因
        self.created_at = time.time()
        self.updated_at = time.time()

    def on_confirm(self):
        """收到确认回报"""
        if self.status == "已提交":
            self.status = "已确认"
            self.updated_at = time.time()

    def on_execution(self, exec_id: str, exec_qty: int, exec_price: float):
        """收到成交回报"""
        self.fills.append({
            "execId": exec_id,
            "execQty": exec_qty,
            "execPrice": exec_price,
        })
        self.filled_qty += exec_qty
        # 更新加权平均价
        total_value = sum(f["execQty"] * f["execPrice"] for f in self.fills)
        self.avg_price = total_value / self.filled_qty if self.filled_qty > 0 else 0.0
        # 更新状态
        if self.filled_qty >= self.qty:
            self.status = "完全成交"
        else:
            self.status = "部分成交"
        self.updated_at = time.time()

    def on_reject(self, reject_code: int, reject_text: str):
        """收到拒绝回报"""
        self.status = "已拒绝"
        self.reject_reason = reject_text
        self.updated_at = time.time()

    def on_cancel(self):
        """收到撤单确认"""
        if self.status not in ("完全成交", "已拒绝"):
            self.status = "已撤单"
            self.updated_at = time.time()

    def to_dict(self) -> dict:
        """导出为字典供前端展示"""
        return {
            "clOrderId": self.cl_order_id,
            "market": self.market,
            "securityId": self.security_id,
            "side": self.side,
            "sideText": "买入" if self.side == "B" else "卖出",
            "price": self.price,
            "qty": self.qty,
            "filledQty": self.filled_qty,
            "remainQty": self.qty - self.filled_qty,
            "avgPrice": round(self.avg_price, 4),
            "status": self.status,
            "fillCount": len(self.fills),
            "fills": self.fills,
            "progress": round(self.filled_qty / self.qty * 100, 1) if self.qty > 0 else 0,
            "shareholderId": self.shareholder_id,
            "rejectReason": self.reject_reason,
            "createdAt": self.created_at,
            "updatedAt": self.updated_at,
        }


class AppState:
    """应用全局状态"""

    def __init__(self):
        self.bridge = TcpBridge(host="127.0.0.1", port=9900)
        self.websocket_clients: list[WebSocket] = []
        self.order_counter = 0

        # 回报历史（内存存储，供前端查询）
        self.responses: list[dict] = []       # gateway 回报
        self.exchange_responses: list[dict] = []  # exchange 回报
        self.max_history = 1000

        # 订单跟踪：clOrderId -> OrderTracker
        self.orders: dict[str, OrderTracker] = {}

    def generate_order_id(self) -> str:
        """生成唯一订单号"""
        self.order_counter += 1
        return f"ADMIN-{int(time.time())}-{self.order_counter:04d}"

    def track_order(self, cl_order_id: str, market: str, security_id: str,
                    side: str, price: float, qty: int, shareholder_id: str):
        """下单时开始跟踪"""
        self.orders[cl_order_id] = OrderTracker(
            cl_order_id, market, security_id, side, price, qty, shareholder_id
        )

    def add_response(self, msg: dict):
        """记录回报并更新订单跟踪状态"""
        source = msg.get("source", "gateway")

        if source == "exchange":
            self.exchange_responses.append(msg)
            if len(self.exchange_responses) > self.max_history:
                self.exchange_responses = self.exchange_responses[-self.max_history:]
        else:
            self.responses.append(msg)
            if len(self.responses) > self.max_history:
                self.responses = self.responses[-self.max_history:]

        # 更新对应订单的跟踪状态
        cl_order_id = msg.get("clOrderId", "")
        tracker = self.orders.get(cl_order_id)
        if tracker:
            if "rejectCode" in msg and msg["rejectCode"] != 0:
                tracker.on_reject(msg["rejectCode"], msg.get("rejectText", ""))
            elif "execId" in msg:
                tracker.on_execution(
                    msg["execId"],
                    msg.get("execQty", 0),
                    msg.get("execPrice", 0.0),
                )
            elif "origClOrderId" in msg:
                # 撤单确认 — 更新原始订单
                orig_tracker = self.orders.get(msg.get("origClOrderId", ""))
                if orig_tracker:
                    orig_tracker.on_cancel()
            else:
                # 确认回报（无 execId、无 rejectCode）
                tracker.on_confirm()


state = AppState()


# ======================== 生命周期 ========================


@asynccontextmanager
async def lifespan(app: FastAPI):
    """应用启动/关闭时管理 TCP 连接"""
    # 启动时连接 C++
    state.bridge.on_response = on_cpp_response
    try:
        await state.bridge.connect()
        logger.info("Connected to C++ AdminServer")
    except Exception as e:
        logger.warning(f"Failed to connect to C++ AdminServer: {e}")
        logger.warning("Running in offline mode (no C++ backend)")

    yield

    # 关闭时断开
    await state.bridge.disconnect()


app = FastAPI(
    title="HDF 撮合系统管理界面",
    description="模拟股票交易对敲撮合系统 — 管理后端 API",
    version="0.1.0",
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


# ======================== 回报处理 ========================


def on_cpp_response(msg: dict):
    """收到 C++ 回报时的回调"""
    state.add_response(msg)
    # 广播给所有 WebSocket 客户端
    asyncio.create_task(broadcast_to_websockets(msg))


async def broadcast_to_websockets(msg: dict):
    """向所有连接的 WebSocket 客户端推送消息"""
    disconnected = []
    for ws in state.websocket_clients:
        try:
            await ws.send_json(msg)
        except Exception:
            disconnected.append(ws)
    for ws in disconnected:
        state.websocket_clients.remove(ws)


# ======================== REST API ========================


@app.post("/api/order", summary="提交订单")
async def submit_order(req: OrderRequest):
    """
    提交一笔新订单到撮合系统。
    自动生成 clOrderId。target 可选 gateway/exchange。
    """
    cl_order_id = state.generate_order_id()
    try:
        await state.bridge.send_order(
            cl_order_id=cl_order_id,
            market=req.market,
            security_id=req.securityId,
            side=req.side,
            price=req.price,
            qty=req.qty,
            shareholder_id=req.shareholderId,
            target=req.target,
        )
        # 开始跟踪此订单
        state.track_order(
            cl_order_id, req.market, req.securityId,
            req.side, req.price, req.qty, req.shareholderId,
        )
        return {"status": "submitted", "clOrderId": cl_order_id, "target": req.target}
    except ConnectionError:
        return {"status": "error", "message": "未连接到撮合系统"}


@app.post("/api/cancel", summary="提交撤单")
async def submit_cancel(req: CancelRequest):
    """
    提交一笔撤单请求。
    自动生成撤单的 clOrderId。target 可选 gateway/exchange。
    """
    cl_order_id = state.generate_order_id()
    try:
        await state.bridge.send_cancel(
            cl_order_id=cl_order_id,
            orig_cl_order_id=req.origClOrderId,
            market=req.market,
            security_id=req.securityId,
            shareholder_id=req.shareholderId,
            side=req.side,
            target=req.target,
        )
        return {"status": "submitted", "clOrderId": cl_order_id, "target": req.target}
    except ConnectionError:
        return {"status": "error", "message": "未连接到撮合系统"}


@app.get("/api/responses", summary="获取回报历史")
async def get_responses(limit: int = 50):
    """获取最近的回报记录"""
    return {"responses": state.responses[-limit:]}


@app.get("/api/orders", summary="订单跟踪列表")
async def get_orders(status: Optional[str] = None):
    """
    获取所有已提交订单的跟踪状态。
    可选按状态过滤：已提交/已确认/部分成交/完全成交/已拒绝/已撤单
    """
    orders = [t.to_dict() for t in state.orders.values()]
    if status:
        orders = [o for o in orders if o["status"] == status]
    # 按创建时间倒序
    orders.sort(key=lambda o: o["createdAt"], reverse=True)
    return {"orders": orders}


@app.get("/api/orders/{cl_order_id}", summary="单个订单详情")
async def get_order_detail(cl_order_id: str):
    """获取指定订单的详细跟踪信息，包含所有成交明细"""
    tracker = state.orders.get(cl_order_id)
    if not tracker:
        return {"error": "订单不存在", "clOrderId": cl_order_id}
    return tracker.to_dict()


@app.get("/api/market", summary="市场行情数据")
async def get_market(security_id: Optional[str] = None):
    """
    返回市场深度（买卖盘口）和最近成交记录。
    优先从 C++ MatchingEngine 获取真实订单簿快照，
    若 C++ 不可用则从 Python 端订单跟踪状态推算。
    """
    bid_depth = []
    ask_depth = []

    # 尝试从 C++ 获取真实订单簿快照
    if state.bridge.is_connected:
        try:
            snapshot = await state.bridge.query_orderbook()
            if snapshot:
                # 使用交易所前置系统的订单簿
                exchange_book = snapshot.get("gateway", {})
                bid_depth = exchange_book.get("bids", [])
                ask_depth = exchange_book.get("asks", [])
        except Exception as e:
            logger.warning(f"Failed to query C++ orderbook: {e}")

    # 回退：从 Python 端订单跟踪状态推算
    if not bid_depth and not ask_depth:
        from collections import defaultdict
        bid_levels: dict[float, int] = defaultdict(int)
        ask_levels: dict[float, int] = defaultdict(int)

        for tracker in state.orders.values():
            if tracker.status not in ("已确认", "部分成交", "已提交"):
                continue
            if security_id and tracker.security_id != security_id:
                continue
            remain = tracker.qty - tracker.filled_qty
            if remain <= 0:
                continue
            if tracker.side == "B":
                bid_levels[tracker.price] += remain
            else:
                ask_levels[tracker.price] += remain

        bid_prices = sorted(bid_levels.keys(), reverse=True)
        cum = 0
        for p in bid_prices:
            cum += bid_levels[p]
            bid_depth.append({"price": p, "qty": bid_levels[p], "cumQty": cum})

        ask_prices = sorted(ask_levels.keys())
        cum = 0
        for p in ask_prices:
            cum += ask_levels[p]
            ask_depth.append({"price": p, "qty": ask_levels[p], "cumQty": cum})

    # 成交记录（始终从回报历史构建）
    trades = []
    for r in state.responses:
        if "execId" not in r:
            continue
        if security_id and r.get("securityId") != security_id:
            continue
        trades.append({
            "execId": r["execId"],
            "clOrderId": r.get("clOrderId", ""),
            "securityId": r.get("securityId", ""),
            "market": r.get("market", ""),
            "side": r.get("side", ""),
            "execQty": r.get("execQty", 0),
            "execPrice": r.get("execPrice", 0.0),
            "shareholderId": r.get("shareholderId", ""),
        })

    last_price = trades[-1]["execPrice"] if trades else None

    securities = list(set(
        t.security_id for t in state.orders.values()
    ))

    return {
        "bidDepth": bid_depth,
        "askDepth": ask_depth,
        "trades": trades,
        "lastPrice": last_price,
        "securities": sorted(securities),
    }


@app.get("/api/status", summary="系统状态")
async def get_status():
    """获取系统连接状态和基本统计"""
    exec_count = sum(1 for r in state.responses if "execId" in r)
    reject_count = sum(1 for r in state.responses if "rejectCode" in r)
    cancel_count = sum(
        1
        for r in state.responses
        if "origClOrderId" in r and "rejectCode" not in r
    )
    confirm_count = len(state.responses) - exec_count - reject_count - cancel_count

    return {
        "connected": state.bridge.is_connected,
        "totalResponses": len(state.responses),
        "executions": exec_count,
        "rejects": reject_count,
        "cancels": cancel_count,
        "confirms": confirm_count,
    }


@app.get("/api/exchange/responses", summary="交易所回报历史")
async def get_exchange_responses(limit: int = 50):
    """获取交易所侧的回报记录"""
    return {"responses": state.exchange_responses[-limit:]}


@app.get("/api/exchange/status", summary="交易所状态")
async def get_exchange_status():
    """获取交易所侧的统计信息"""
    resps = state.exchange_responses
    exec_count = sum(1 for r in resps if "execId" in r)
    reject_count = sum(1 for r in resps if "rejectCode" in r)
    cancel_count = sum(
        1 for r in resps
        if "origClOrderId" in r and "rejectCode" not in r
    )
    confirm_count = len(resps) - exec_count - reject_count - cancel_count

    return {
        "totalResponses": len(resps),
        "executions": exec_count,
        "rejects": reject_count,
        "cancels": cancel_count,
        "confirms": confirm_count,
    }


# ======================== WebSocket ========================


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    """
    WebSocket 端点 — 实时推送回报给前端。
    连接后自动接收所有新回报。
    """
    await ws.accept()
    state.websocket_clients.append(ws)
    logger.info(f"WebSocket client connected ({len(state.websocket_clients)} total)")

    try:
        while True:
            # 保持连接，接收前端心跳或关闭
            data = await ws.receive_text()
            # 可扩展：接收前端的过滤条件等
    except WebSocketDisconnect:
        pass
    finally:
        if ws in state.websocket_clients:
            state.websocket_clients.remove(ws)
        logger.info(
            f"WebSocket client disconnected ({len(state.websocket_clients)} remaining)"
        )
