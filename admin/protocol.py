"""
TCP 协议定义 — C++ TradeSystem ↔ Python 管理界面

通信协议：
  - 传输层：TCP
  - 消息格式：每条消息为一个 JSON 对象，以换行符 '\n' 分隔（JSON Lines）
  - 编码：UTF-8

消息类型（type 字段区分）：

  Python → C++:
    - order:    提交订单
    - cancel:   提交撤单
    - query:    查询请求（订单簿快照等）

  C++ → Python:
    - response: 回报（确认/成交/拒绝/撤单确认）
    - snapshot: 订单簿快照
    - error:    错误信息
"""

from dataclasses import dataclass, field, asdict
from enum import Enum
from typing import Optional
import json


# ======================== 枚举 ========================

class MessageType(str, Enum):
    """消息类型"""
    ORDER = "order"
    CANCEL = "cancel"
    QUERY = "query"
    RESPONSE = "response"
    SNAPSHOT = "snapshot"
    ERROR = "error"


class Side(str, Enum):
    BUY = "B"
    SELL = "S"


class Market(str, Enum):
    XSHG = "XSHG"
    XSHE = "XSHE"
    BJSE = "BJSE"


# ======================== 消息结构 ========================

@dataclass
class OrderMessage:
    """Python → C++：提交订单"""
    clOrderId: str
    market: str
    securityId: str
    side: str
    price: float
    qty: int
    shareholderId: str

    def to_wire(self) -> str:
        payload = {"type": MessageType.ORDER, **asdict(self)}
        return json.dumps(payload, ensure_ascii=False)


@dataclass
class CancelMessage:
    """Python → C++：提交撤单"""
    clOrderId: str
    origClOrderId: str
    market: str
    securityId: str
    shareholderId: str
    side: str

    def to_wire(self) -> str:
        payload = {"type": MessageType.CANCEL, **asdict(self)}
        return json.dumps(payload, ensure_ascii=False)


@dataclass
class QueryMessage:
    """Python → C++：查询订单簿快照"""
    queryType: str = "orderbook"  # "orderbook" | "stats"

    def to_wire(self) -> str:
        payload = {"type": MessageType.QUERY, "queryType": self.queryType}
        return json.dumps(payload, ensure_ascii=False)


@dataclass
class ResponseMessage:
    """C++ → Python：回报（确认/成交/拒绝/撤单）"""
    data: dict = field(default_factory=dict)

    @classmethod
    def from_wire(cls, raw: str) -> "ResponseMessage":
        obj = json.loads(raw)
        return cls(data=obj)

    @property
    def is_exec(self) -> bool:
        return "execId" in self.data

    @property
    def is_reject(self) -> bool:
        return "rejectCode" in self.data

    @property
    def is_cancel(self) -> bool:
        return "origClOrderId" in self.data and not self.is_reject

    @property
    def is_confirm(self) -> bool:
        return not self.is_exec and not self.is_reject and not self.is_cancel


def parse_message(raw: str) -> dict:
    """解析一条 JSON Lines 消息"""
    return json.loads(raw)
