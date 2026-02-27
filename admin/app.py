"""
app.py — Streamlit 管理界面前端

功能：
  1. 仪表盘 — 系统状态概览
  2. 订单簿 — 实时买卖盘口展示
  3. 成交记录 — 最近成交流水
  4. 手动下单 — 提交订单/撤单表单
  5. 风控日志 — 对敲拦截等拒绝记录

启动方式：
  conda activate hdf
  streamlit run app.py

TODO: 由组员完善各页面的具体展示逻辑
"""

import time

import requests
import streamlit as st

# ======================== 配置 ========================

API_BASE = "http://127.0.0.1:8000"

st.set_page_config(
    page_title="HDF 撮合系统管理界面",
    page_icon="📊",
    layout="wide",
)


# ======================== 工具函数 ========================


def api_get(path: str, **params):
    """调用后端 GET API"""
    try:
        resp = requests.get(f"{API_BASE}{path}", params=params, timeout=3)
        resp.raise_for_status()
        return resp.json()
    except requests.RequestException as e:
        st.error(f"API 请求失败: {e}")
        return None


def api_post(path: str, data: dict):
    """调用后端 POST API"""
    try:
        resp = requests.post(f"{API_BASE}{path}", json=data, timeout=3)
        resp.raise_for_status()
        return resp.json()
    except requests.RequestException as e:
        st.error(f"API 请求失败: {e}")
        return None


# ======================== 侧边栏导航 ========================

st.sidebar.title("📊 HDF 管理界面")
page = st.sidebar.radio(
    "导航",
    ["仪表盘", "成交记录", "手动下单", "手动撤单", "风控日志"],
    index=0,
)

# 系统状态（侧边栏底部）
st.sidebar.markdown("---")
status = api_get("/api/status")
if status:
    color = "🟢" if status["connected"] else "🔴"
    st.sidebar.markdown(f"{color} 撮合系统: {'已连接' if status['connected'] else '未连接'}")
    st.sidebar.caption(f"总回报: {status['totalResponses']}")


# ======================== 页面：仪表盘 ========================

if page == "仪表盘":
    st.title("📊 系统仪表盘")

    if status:
        col1, col2, col3, col4 = st.columns(4)
        col1.metric("成交笔数", status.get("executions", 0))
        col2.metric("确认回报", status.get("confirms", 0))
        col3.metric("撤单确认", status.get("cancels", 0))
        col4.metric("拒绝/风控", status.get("rejects", 0))
    else:
        st.warning("无法获取系统状态")

    st.markdown("---")
    st.subheader("最近回报")

    # TODO: 组员实现
    # 1. 调用 api_get("/api/responses", limit=20)
    # 2. 将回报按类型分类展示（确认/成交/拒绝/撤单）
    # 3. 使用 st.dataframe 或 st.table 展示
    # 4. 考虑添加 st.auto_refresh 或 st.button("刷新") 实现实时更新

    responses = api_get("/api/responses", limit=20)
    if responses and responses.get("responses"):
        st.json(responses["responses"])
    else:
        st.info("暂无回报记录")


# ======================== 页面：成交记录 ========================

elif page == "成交记录":
    st.title("💹 成交记录")

    # TODO: 组员实现
    # 1. 从 /api/responses 获取所有回报
    # 2. 过滤出含 execId 的成交回报
    # 3. 展示字段：execId, clOrderId, side, securityId, execQty, execPrice, market
    # 4. 按时间倒序排列
    # 5. 可选：添加搜索/过滤功能（按证券代码、方向等）

    responses = api_get("/api/responses", limit=200)
    if responses:
        exec_reports = [
            r for r in responses.get("responses", []) if "execId" in r
        ]
        if exec_reports:
            st.dataframe(exec_reports, use_container_width=True)
        else:
            st.info("暂无成交记录")

    if st.button("🔄 刷新"):
        st.rerun()


# ======================== 页面：手动下单 ========================

elif page == "手动下单":
    st.title("📝 手动下单")

    with st.form("order_form"):
        col1, col2 = st.columns(2)

        with col1:
            market = st.selectbox("市场", ["XSHG", "XSHE", "BJSE"])
            security_id = st.text_input("证券代码", value="600030")
            side = st.selectbox("方向", ["B", "S"], format_func=lambda x: "买入" if x == "B" else "卖出")

        with col2:
            price = st.number_input("价格", min_value=0.01, value=10.0, step=0.01)
            qty = st.number_input("数量", min_value=100, value=100, step=100)
            shareholder_id = st.text_input("股东号", value="SH001")

        submitted = st.form_submit_button("提交订单", type="primary")

        if submitted:
            result = api_post(
                "/api/order",
                {
                    "market": market,
                    "securityId": security_id,
                    "side": side,
                    "price": price,
                    "qty": int(qty),
                    "shareholderId": shareholder_id,
                },
            )
            if result:
                if result.get("status") == "submitted":
                    st.success(f"✅ 订单已提交，编号: {result['clOrderId']}")
                else:
                    st.error(f"❌ 提交失败: {result.get('message', '未知错误')}")

    # ======================== 订单跟踪 ========================
    st.markdown("---")
    st.subheader("📋 我的订单")

    # 状态筛选
    filter_col1, filter_col2 = st.columns([1, 3])
    with filter_col1:
        status_filter = st.selectbox(
            "状态筛选",
            ["全部", "已提交", "已确认", "部分成交", "完全成交", "已拒绝", "已撤单"],
        )
    with filter_col2:
        if st.button("🔄 刷新订单"):
            st.rerun()

    # 获取订单列表
    params = {}
    if status_filter != "全部":
        params["status"] = status_filter
    orders_data = api_get("/api/orders", **params)

    if orders_data and orders_data.get("orders"):
        orders = orders_data["orders"]

        for order in orders:
            # 根据状态选择颜色和图标
            status = order["status"]
            progress = order["progress"]
            if status == "完全成交":
                icon, color = "✅", "green"
            elif status == "部分成交":
                icon, color = "🔶", "orange"
            elif status == "已拒绝":
                icon, color = "❌", "red"
            elif status == "已撤单":
                icon, color = "🚫", "gray"
            elif status == "已确认":
                icon, color = "📩", "blue"
            else:
                icon, color = "⏳", "blue"

            side_text = order["sideText"]
            side_emoji = "🟢" if order["side"] == "B" else "🔴"

            with st.container():
                # 标题行
                header_col1, header_col2, header_col3 = st.columns([3, 1, 1])
                with header_col1:
                    st.markdown(
                        f"**{side_emoji} {side_text} {order['securityId']}** "
                        f"({order['market']}) — `{order['clOrderId']}`"
                    )
                with header_col2:
                    st.markdown(f":{color}[**{icon} {status}**]")
                with header_col3:
                    st.markdown(f"**{progress}%** 成交")

                # 进度条
                st.progress(min(progress / 100.0, 1.0))

                # 详情
                detail_col1, detail_col2, detail_col3, detail_col4 = st.columns(4)
                with detail_col1:
                    st.metric("委托价", f"¥{order['price']:.2f}")
                with detail_col2:
                    st.metric("委托量", f"{order['qty']}")
                with detail_col3:
                    st.metric("已成交", f"{order['filledQty']}")
                with detail_col4:
                    avg_price_str = f"¥{order['avgPrice']:.2f}" if order['avgPrice'] > 0 else "—"
                    st.metric("成交均价", avg_price_str)

                # 成交明细展开
                if order["fills"]:
                    with st.expander(f"📄 成交明细 ({order['fillCount']} 笔)"):
                        for i, fill in enumerate(order["fills"], 1):
                            st.text(
                                f"  {i}. {fill['execId']}  "
                                f"数量: {fill['execQty']}  "
                                f"价格: ¥{fill['execPrice']:.2f}"
                            )

                st.markdown("---")
    else:
        st.info("暂无订单记录，提交订单后将在此显示跟踪状态。")


# ======================== 页面：手动撤单 ========================

elif page == "手动撤单":
    st.title("🚫 手动撤单")

    with st.form("cancel_form"):
        orig_order_id = st.text_input("原始订单号 (origClOrderId)", value="")
        col1, col2 = st.columns(2)

        with col1:
            market = st.selectbox("市场", ["XSHG", "XSHE", "BJSE"])
            security_id = st.text_input("证券代码", value="600030")

        with col2:
            side = st.selectbox("方向", ["B", "S"], format_func=lambda x: "买入" if x == "B" else "卖出")
            shareholder_id = st.text_input("股东号", value="SH001")

        submitted = st.form_submit_button("提交撤单", type="primary")

        if submitted:
            if not orig_order_id:
                st.error("请输入原始订单号")
            else:
                result = api_post(
                    "/api/cancel",
                    {
                        "origClOrderId": orig_order_id,
                        "market": market,
                        "securityId": security_id,
                        "shareholderId": shareholder_id,
                        "side": side,
                    },
                )
                if result:
                    if result.get("status") == "submitted":
                        st.success(f"✅ 撤单已提交，编号: {result['clOrderId']}")
                    else:
                        st.error(f"❌ 撤单失败: {result.get('message', '未知错误')}")


# ======================== 页面：风控日志 ========================

elif page == "风控日志":
    st.title("🛡️ 风控日志")

    # TODO: 组员实现
    # 1. 从 /api/responses 获取所有回报
    # 2. 过滤出含 rejectCode 的拒绝回报
    # 3. 区分对敲拒绝（rejectCode=0x01）和格式错误（rejectCode=0x02）
    # 4. 展示字段：clOrderId, securityId, side, rejectCode, rejectText
    # 5. 可选：按拒绝类型分 tab 展示

    responses = api_get("/api/responses", limit=500)
    if responses:
        rejects = [
            r for r in responses.get("responses", []) if "rejectCode" in r
        ]
        if rejects:
            # 分类
            cross_trades = [r for r in rejects if r.get("rejectCode") == 1]
            format_errors = [r for r in rejects if r.get("rejectCode") == 2]
            others = [r for r in rejects if r.get("rejectCode") not in (1, 2)]

            tab1, tab2, tab3 = st.tabs(
                [f"对敲拦截 ({len(cross_trades)})", f"格式错误 ({len(format_errors)})", f"其他 ({len(others)})"]
            )
            with tab1:
                if cross_trades:
                    st.dataframe(cross_trades, use_container_width=True)
                else:
                    st.info("无对敲拦截记录")
            with tab2:
                if format_errors:
                    st.dataframe(format_errors, use_container_width=True)
                else:
                    st.info("无格式错误记录")
            with tab3:
                if others:
                    st.dataframe(others, use_container_width=True)
                else:
                    st.info("无其他拒绝记录")
        else:
            st.info("暂无风控拦截记录")

    if st.button("🔄 刷新"):
        st.rerun()
