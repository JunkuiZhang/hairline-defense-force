"""
生成模拟交易订单数据，用于 TradeLogger 历史记录生成

输出: data/generated_orders.jsonl
场景覆盖:
  - 3只股票，10个股东
  - 买卖交叉（产生成交）
  - 挂单不成交（价差过大）
  - 同股东对敲（触发风控拒绝）
  - 撤单指令
"""

import json
import random
import os

random.seed(42)  # 可复现

# ============================================================
# 配置
# ============================================================
NUM_ORDERS = 500
CANCEL_RATIO = 0.08  # 8% 的指令是撤单

STOCKS = {
    "600030": {"market": "XSHG", "base": 30.0, "spread": 2.0},   # 中信证券
    "600519": {"market": "XSHG", "base": 1800.0, "spread": 20.0}, # 贵州茅台
    "000001": {"market": "XSHE", "base": 15.0, "spread": 1.5},   # 平安银行
}

SHAREHOLDERS = [f"SH{i:03d}" for i in range(1, 11)]  # SH001 ~ SH010

def run():
    # ============================================================
    # 生成
    # ============================================================
    orders = []
    active_order_ids = []  # 可撤单的订单 ID
    order_seq = 0

    for i in range(NUM_ORDERS):
        # 随机决定是否为撤单
        if active_order_ids and random.random() < CANCEL_RATIO:
            orig_id = random.choice(active_order_ids)
            order_seq += 1
            cancel = {
                "type": "cancel",
                "clOrderId": f"CXL{order_seq:06d}",
                "origClOrderId": orig_id,
                "market": "XSHG",  # 简化
                "securityId": "600030",
                "shareholderId": "SH001",
                "side": "B",
            }
            orders.append(cancel)
            active_order_ids.remove(orig_id)
            continue

        # 新订单
        stock_id = random.choice(list(STOCKS.keys()))
        stock = STOCKS[stock_id]
        side = random.choice(["B", "S"])

        # 价格：基准价 ± 随机偏移，保留2位小数
        price = round(stock["base"] + random.uniform(-stock["spread"], stock["spread"]), 2)
        # 确保价格 > 0
        price = max(0.01, price)

        qty = random.choice([100, 200, 300, 500, 1000])
        shareholder = random.choice(SHAREHOLDERS)

        order_seq += 1
        order_id = f"ORD{order_seq:06d}"

        order = {
            "type": "order",
            "clOrderId": order_id,
            "market": stock["market"],
            "securityId": stock_id,
            "side": side,
            "price": price,
            "qty": qty,
            "shareholderId": shareholder,
        }
        orders.append(order)
        active_order_ids.append(order_id)

    # ============================================================
    # 写入文件
    # ============================================================
    os.makedirs("data", exist_ok=True)
    output_path = "data/generated_orders.jsonl"

    with open(output_path, "w") as f:
        for o in orders:
            f.write(json.dumps(o) + "\n")

    # 统计
    order_count = sum(1 for o in orders if o["type"] == "order")
    cancel_count = sum(1 for o in orders if o["type"] == "cancel")
    print(f"已生成 {len(orders)} 条指令 → {output_path}")
    print(f"  委托: {order_count}, 撤单: {cancel_count}")
    print(f"  股票: {list(STOCKS.keys())}")
    print(f"  股东: {SHAREHOLDERS[0]} ~ {SHAREHOLDERS[-1]}")
    

if __name__ == "__main__":
    run()
