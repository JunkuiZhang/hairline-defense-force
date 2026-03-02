import json

import matplotlib.pyplot as plt
import pandas as pd


def load_history(path: str) -> pd.DataFrame:
    """从 JSONL 文件加载交易历史"""
    records = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                records.append(json.loads(line))
    df = pd.DataFrame(records)
    # 转换时间戳
    if "timestamp" in df.columns:
        df["time"] = pd.to_datetime(df["timestamp"], unit="ms")
    return df


def print_separator(title: str):
    print(f"\n{'='*60}")
    print(f"  {title}")
    print(f"{'='*60}")


def analyze_overview(df: pd.DataFrame):
    """1. 事件概览"""
    print_separator("1. 事件概览")
    counts = df["event"].value_counts()
    total = len(df)
    for event, count in counts.items():
        pct = count / total * 100
        print(f"  {event:<20s} {count:>6d}  ({pct:5.1f}%)")
    print(f"  {'总计':<20s} {total:>6d}")
    return counts


def analyze_executions(df: pd.DataFrame):
    """2. 成交分析 — 只统计 isMaker=True 避免重复计数"""
    execs = df[(df["event"] == "EXECUTION") & (df["isMaker"] == True)].copy()
    if execs.empty:
        print("\n  无成交记录")
        return

    print_separator("2. 成交分析（按证券）")

    execs["turnover"] = execs["execQty"] * execs["execPrice"]
    summary = (
        execs.groupby("securityId")
        .agg(
            成交笔数=("execQty", "count"),
            成交量=("execQty", "sum"),
            成交额=("turnover", "sum"),
            最高价=("execPrice", "max"),
            最低价=("execPrice", "min"),
            均价=("execPrice", "mean"),
        )
        .round(2)
    )
    print(summary.to_string())

    total_vol = execs["execQty"].sum()
    total_turnover = execs["turnover"].sum()
    print(f"\n  总成交量: {total_vol:,}  总成交额: {total_turnover:,.2f}")

    # 图表: 各证券成交额占比
    fig, axes = plt.subplots(1, 2, figsize=(12, 5))

    summary["成交额"].plot.bar(ax=axes[0], color="#2196F3")
    axes[0].set_title("各证券成交额")
    axes[0].set_ylabel("成交额")
    axes[0].tick_params(axis="x", rotation=0)

    summary["成交量"].plot.bar(ax=axes[1], color="#4CAF50")
    axes[1].set_title("各证券成交量")
    axes[1].set_ylabel("成交量（股）")
    axes[1].tick_params(axis="x", rotation=0)

    plt.tight_layout()
    plt.show()
    plt.close()


def analyze_price_trend(df: pd.DataFrame):
    """3. 价格走势"""
    execs = df[(df["event"] == "EXECUTION") & (df["isMaker"] == True)].copy()
    if execs.empty:
        return

    print_separator("3. 价格走势")

    securities = execs["securityId"].unique()
    n = len(securities)
    fig, axes = plt.subplots(n, 1, figsize=(12, 4 * n), squeeze=False)

    for i, sec_id in enumerate(sorted(securities)):
        sec_data = execs[execs["securityId"] == sec_id].sort_values("timestamp")
        ax = axes[i][0]
        ax.plot(range(len(sec_data)), sec_data["execPrice"], ".-", markersize=3, linewidth=0.8)
        ax.set_title(f"{sec_id} 成交价格走势")
        ax.set_xlabel("成交序号")
        ax.set_ylabel("成交价")
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    plt.show()
    plt.close()


def analyze_shareholders(df: pd.DataFrame):
    """4. 股东活跃度分析"""
    print_separator("4. 股东活跃度")

    # 委托量统计
    orders = df[df["event"] == "ORDER_NEW"].copy()
    if orders.empty:
        print("  无委托记录")
        return

    order_stats = (
        orders.groupby("shareholderId")
        .agg(委托笔数=("qty", "count"), 委托量=("qty", "sum"))
    )

    # 成交量统计（主动方 + 被动方都算）
    execs = df[df["event"] == "EXECUTION"].copy()
    if not execs.empty:
        exec_stats = (
            execs.groupby("clOrderId")
            .agg(execQty=("execQty", "first"))
        )
        # 需要从 ORDER_NEW 中获取 shareholderId
        order_map = orders.set_index("clOrderId")["shareholderId"].to_dict()
        execs["shareholderId_mapped"] = execs["clOrderId"].map(order_map)
        exec_by_sh = (
            execs.dropna(subset=["shareholderId_mapped"])
            .groupby("shareholderId_mapped")
            .agg(成交量=("execQty", "sum"), 成交笔数=("execQty", "count"))
        )
        exec_by_sh.index.name = "shareholderId"
        combined = order_stats.join(exec_by_sh, how="left").fillna(0)
        combined["成交量"] = combined["成交量"].astype(int)
        combined["成交笔数"] = combined["成交笔数"].astype(int)
    else:
        combined = order_stats
        combined["成交量"] = 0
        combined["成交笔数"] = 0

    combined = combined.sort_values("委托量", ascending=False)
    print(combined.to_string())

    fig, ax = plt.subplots(figsize=(10, 5))
    x = range(len(combined))
    width = 0.35
    ax.bar([i - width / 2 for i in x], combined["委托量"], width, label="委托量", color="#2196F3")
    ax.bar([i + width / 2 for i in x], combined["成交量"], width, label="成交量", color="#FF9800")
    ax.set_xticks(list(x))
    ax.set_xticklabels(combined.index, rotation=45)
    ax.set_title("各股东 委托量 vs 成交量")
    ax.set_ylabel("数量（股）")
    ax.legend()
    plt.tight_layout()
    plt.show()
    plt.close()

def analyze_rejections(df: pd.DataFrame):
    """5. 拒绝分析"""
    print_separator("5. 拒绝分析")

    rejects = df[df["event"] == "ORDER_REJECT"]
    orders = df[df["event"] == "ORDER_NEW"]

    total_orders = len(orders)
    total_rejects = len(rejects)

    if total_orders == 0:
        print("  无委托记录")
        return

    reject_rate = total_rejects / total_orders * 100
    print(f"  总委托: {total_orders}, 拒绝: {total_rejects}, 拒绝率: {reject_rate:.1f}%")

    if total_rejects > 0:
        print("\n  拒绝原因分布:")
        reasons = rejects["rejectText"].value_counts()
        for reason, count in reasons.items():
            pct = count / total_rejects * 100
            print(f"    {reason:<40s} {count:>4d}  ({pct:5.1f}%)")

        fig, ax = plt.subplots(figsize=(8, 5))
        reasons.plot.barh(ax=ax, color="#F44336")
        ax.set_title("拒绝原因分布")
        ax.set_xlabel("次数")
        plt.tight_layout()
        plt.show()
        plt.close()


def analyze_cancels(df: pd.DataFrame):
    """6. 撤单分析"""
    print_separator("6. 撤单分析")

    confirms = df[df["event"] == "CANCEL_CONFIRM"]
    rejects = df[df["event"] == "CANCEL_REJECT"]

    total = len(confirms) + len(rejects)
    if total == 0:
        print("  无撤单记录")
        return

    success_rate = len(confirms) / total * 100
    print(f"  撤单请求: {total}")
    print(f"  撤单成功: {len(confirms)} ({success_rate:.1f}%)")
    print(f"  撤单拒绝: {len(rejects)} ({100 - success_rate:.1f}%)")

    if not confirms.empty:
        total_canceled = confirms["canceledQty"].sum()
        total_cum = confirms["cumQty"].sum()
        print(f"  撤回总量: {total_canceled:,} 股")
        print(f"  已成交量: {total_cum:,} 股（撤单前已部分成交）")


def analyze_sides(df: pd.DataFrame):
    """7. 买卖方向分析"""
    print_separator("7. 买卖方向分析")

    orders = df[df["event"] == "ORDER_NEW"].copy()
    if orders.empty:
        print("  无委托记录")
        return

    side_stats = (
        orders.groupby("side")
        .agg(委托笔数=("qty", "count"), 委托量=("qty", "sum"))
    )
    side_stats.index = side_stats.index.map({"B": "买入(B)", "S": "卖出(S)"})
    print(side_stats.to_string())

    # 成交方向
    execs = df[(df["event"] == "EXECUTION") & (df["isMaker"] == True)].copy()
    if not execs.empty:
        exec_sides = (
            execs.groupby("side")
            .agg(成交笔数=("execQty", "count"), 成交量=("execQty", "sum"))
        )
        exec_sides.index = exec_sides.index.map({"B": "买入(B)", "S": "卖出(S)"})
        print("\n  被动方（maker）成交方向:")
        print(exec_sides.to_string())

    fig, axes = plt.subplots(1, 2, figsize=(10, 4))

    side_stats["委托量"].plot.pie(ax=axes[0], autopct="%1.1f%%", colors=["#F44336", "#4CAF50"])
    axes[0].set_title("委托量买卖占比")
    axes[0].set_ylabel("")

    if not execs.empty:
        exec_sides["成交量"].plot.pie(ax=axes[1], autopct="%1.1f%%", colors=["#F44336", "#4CAF50"])
        axes[1].set_title("成交量买卖占比")
        axes[1].set_ylabel("")

    plt.tight_layout()
    plt.show()
    plt.close()


def analyze_price_distribution(df: pd.DataFrame):
    """8. 成交价格分布"""
    execs = df[(df["event"] == "EXECUTION") & (df["isMaker"] == True)].copy()
    if execs.empty:
        return

    print_separator("8. 成交价格分布")

    securities = sorted(execs["securityId"].unique())
    n = len(securities)
    fig, axes = plt.subplots(n, 1, figsize=(8, 4 * n), squeeze=False)

    for i, sec_id in enumerate(securities):
        sec_data = execs[execs["securityId"] == sec_id]
        ax = axes[i][0]
        ax.hist(sec_data["execPrice"], bins=20, color="#9C27B0", edgecolor="white", alpha=0.8)
        ax.set_title(f"{sec_id} 成交价分布")
        ax.set_xlabel("成交价")
        ax.set_ylabel("笔数")
        ax.axvline(sec_data["execPrice"].mean(), color="red", linestyle="--", label=f'均价 {sec_data["execPrice"].mean():.2f}')
        ax.legend(fontsize=8)

    plt.tight_layout()
    plt.show()
    plt.close()

    plt.close()
