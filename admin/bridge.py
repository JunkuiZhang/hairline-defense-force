"""
bridge.py — C++ TradeSystem TCP 桥接模块

负责：
  - 与 C++ AdminServer 建立 TCP 连接
  - 发送订单/撤单/查询请求
  - 异步接收回报并通过回调分发

使用方法：
  bridge = TcpBridge("127.0.0.1", 9900)
  bridge.on_response = lambda msg: print(msg)
  bridge.connect()
  bridge.send_order({...})
  bridge.disconnect()
"""

import asyncio
import json
import logging
from typing import Callable, Optional

logger = logging.getLogger(__name__)


class TcpBridge:
    """与 C++ AdminServer 的 TCP 连接桥接"""

    def __init__(self, host: str = "127.0.0.1", port: int = 9900):
        self.host = host
        self.port = port
        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None
        self._recv_task: Optional[asyncio.Task] = None

        # 回调：收到 C++ 回报时调用
        self.on_response: Optional[Callable[[dict], None]] = None

        # 查询请求的待处理 Future：收到 snapshot 回复时完成
        self._pending_queries: dict[str, asyncio.Future] = {}

    async def connect(self):
        """建立 TCP 连接并启动接收循环"""
        logger.info(f"Connecting to C++ AdminServer at {self.host}:{self.port}")
        self._reader, self._writer = await asyncio.open_connection(
            self.host, self.port
        )
        self._recv_task = asyncio.create_task(self._receive_loop())
        logger.info("Connected successfully")

    async def disconnect(self):
        """断开连接"""
        if self._recv_task:
            self._recv_task.cancel()
            self._recv_task = None
        if self._writer:
            self._writer.close()
            await self._writer.wait_closed()
            self._writer = None
            self._reader = None
        logger.info("Disconnected from C++ AdminServer")

    @property
    def is_connected(self) -> bool:
        return self._writer is not None and not self._writer.is_closing()

    async def send_order(
        self,
        cl_order_id: str,
        market: str,
        security_id: str,
        side: str,
        price: float,
        qty: int,
        shareholder_id: str,
        target: str = "gateway",
    ):
        """发送订单到 C++ TradeSystem"""
        msg = {
            "type": "order",
            "clOrderId": cl_order_id,
            "market": market,
            "securityId": security_id,
            "side": side,
            "price": price,
            "qty": qty,
            "shareholderId": shareholder_id,
            "target": target,
        }
        await self._send(msg)

    async def send_cancel(
        self,
        cl_order_id: str,
        orig_cl_order_id: str,
        market: str,
        security_id: str,
        shareholder_id: str,
        side: str,
        target: str = "gateway",
    ):
        """发送撤单到 C++ TradeSystem"""
        msg = {
            "type": "cancel",
            "clOrderId": cl_order_id,
            "origClOrderId": orig_cl_order_id,
            "market": market,
            "securityId": security_id,
            "shareholderId": shareholder_id,
            "side": side,
            "target": target,
        }
        await self._send(msg)

    async def query_orderbook(self) -> dict:
        """请求订单簿快照，等待 C++ 端返回结果"""
        loop = asyncio.get_event_loop()
        future = loop.create_future()
        self._pending_queries["orderbook"] = future
        msg = {"type": "query", "queryType": "orderbook"}
        await self._send(msg)
        try:
            return await asyncio.wait_for(future, timeout=3.0)
        except asyncio.TimeoutError:
            self._pending_queries.pop("orderbook", None)
            logger.warning("Orderbook query timed out")
            return {}

    async def query_stats(self) -> dict:
        """请求统计信息，等待 C++ 端返回结果"""
        loop = asyncio.get_event_loop()
        future = loop.create_future()
        self._pending_queries["stats"] = future
        msg = {"type": "query", "queryType": "stats"}
        await self._send(msg)
        try:
            return await asyncio.wait_for(future, timeout=3.0)
        except asyncio.TimeoutError:
            self._pending_queries.pop("stats", None)
            logger.warning("Stats query timed out")
            return {}

    async def _send(self, msg: dict):
        """发送 JSON Lines 消息"""
        if not self.is_connected:
            raise ConnectionError("Not connected to C++ AdminServer")
        line = json.dumps(msg, ensure_ascii=False) + "\n"
        self._writer.write(line.encode("utf-8"))
        await self._writer.drain()
        logger.debug(f"Sent: {msg.get('type', '?')}")

    async def _receive_loop(self):
        """持续接收 C++ 回报"""
        try:
            while True:
                line = await self._reader.readline()
                if not line:
                    logger.warning("Connection closed by C++ server")
                    break
                try:
                    msg = json.loads(line.decode("utf-8").strip())
                    logger.debug(f"Received: {msg.get('type', '?')}")
                    # 查询回复：解析 pending query
                    query_type = msg.get("queryType")
                    if query_type and query_type in self._pending_queries:
                        future = self._pending_queries.pop(query_type)
                        if not future.done():
                            future.set_result(msg)
                    elif self.on_response:
                        self.on_response(msg)
                except json.JSONDecodeError as e:
                    logger.error(f"Invalid JSON from server: {e}")
        except asyncio.CancelledError:
            pass
        except Exception as e:
            logger.error(f"Receive loop error: {e}")
