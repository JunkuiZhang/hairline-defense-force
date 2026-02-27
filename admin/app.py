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

import pandas as pd
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
    ["仪表盘", "市场行情", "手动下单", "手动撤单", "交易所监控", "风控日志"],
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


# ======================== 页面：市场行情 ========================

elif page == "市场行情":
    st.title("📈 市场行情")

    # 证券筛选
    market_data = api_get("/api/market")
    if not market_data:
        st.warning("无法获取市场数据")
    else:
        # 证券选择
        securities = market_data.get("securities", [])
        filter_col1, filter_col2 = st.columns([1, 3])
        with filter_col1:
            sec_options = ["全部"] + securities
            sec_filter = st.selectbox("证券代码", sec_options)
        with filter_col2:
            if st.button("🔄 刷新行情"):
                st.rerun()

        # 如果选了具体证券，重新请求
        if sec_filter != "全部":
            market_data = api_get("/api/market", security_id=sec_filter) or market_data

        bid_depth = market_data.get("bidDepth", [])
        ask_depth = market_data.get("askDepth", [])
        trades = market_data.get("trades", [])
        last_price = market_data.get("lastPrice")

        # ---- 概览指标 ----
        m1, m2, m3, m4 = st.columns(4)
        m1.metric("最新成交价", f"¥{last_price:.2f}" if last_price else "—")
        m2.metric("买盘挂单量", sum(b["qty"] for b in bid_depth))
        m3.metric("卖盘挂单量", sum(a["qty"] for a in ask_depth))
        m4.metric("总成交笔数", len(trades))

        # ---- 深度图（Steam 风格） ----
        st.subheader("🏔️ 市场深度")

        if bid_depth or ask_depth:
            import altair as alt

            layers = []

            # 买盘：独立 DataFrame，价格从低到高，cumQty 递减（从高价最大到低价最小）
            if bid_depth:
                bid_rows = []
                for b in reversed(bid_depth):  # 低价 → 高价
                    bid_rows.append({"价格": b["price"], "累积量": b["cumQty"]})
                df_bid = pd.DataFrame(bid_rows)

                bid_base = alt.Chart(df_bid).encode(
                    x=alt.X("价格:Q", title="价格 (¥)", scale=alt.Scale(zero=False)),
                )
                bid_area = bid_base.mark_area(
                    interpolate="monotone", opacity=0.35, color="#22c55e",
                ).encode(
                    y=alt.Y("累积量:Q", title="累积数量"),
                    tooltip=[alt.Tooltip("价格:Q", title="价格"),
                             alt.Tooltip("累积量:Q", title="买盘累积")],
                )
                bid_line = bid_base.mark_line(
                    interpolate="monotone", color="#16a34a", strokeWidth=2,
                ).encode(y="累积量:Q")
                layers += [bid_area, bid_line]

            # 卖盘：独立 DataFrame，价格从低到高，cumQty 递增
            if ask_depth:
                ask_rows = []
                for a in ask_depth:  # 低价 → 高价
                    ask_rows.append({"价格": a["price"], "累积量": a["cumQty"]})
                df_ask = pd.DataFrame(ask_rows)

                ask_base = alt.Chart(df_ask).encode(
                    x=alt.X("价格:Q", title="价格 (¥)", scale=alt.Scale(zero=False)),
                )
                ask_area = ask_base.mark_area(
                    interpolate="monotone", opacity=0.35, color="#ef4444",
                ).encode(
                    y=alt.Y("累积量:Q", title="累积数量"),
                    tooltip=[alt.Tooltip("价格:Q", title="价格"),
                             alt.Tooltip("累积量:Q", title="卖盘累积")],
                )
                ask_line = ask_base.mark_line(
                    interpolate="monotone", color="#dc2626", strokeWidth=2,
                ).encode(y="累积量:Q")
                layers += [ask_area, ask_line]

            # 最新成交价竖线
            if last_price:
                price_rule = alt.Chart(
                    pd.DataFrame([{"price": last_price}])
                ).mark_rule(
                    color="#fbbf24", strokeWidth=2, strokeDash=[4, 4]
                ).encode(x="price:Q")
                layers.append(price_rule)

            if layers:
                chart = alt.layer(*layers).resolve_scale(
                    y="shared"
                ).properties(
                    height=350,
                    title="买卖盘深度图",
                ).interactive()

                st.altair_chart(chart, width="stretch")
        else:
            st.info("暂无挂单数据，下单后将在此显示市场深度。")

        # ---- 买卖五档 ----
        st.subheader("📊 买卖盘口")
        book_col1, book_col2 = st.columns(2)

        with book_col1:
            st.markdown("**🟢 买盘 (Bid)**")
            if bid_depth:
                bid_display = []
                for i, b in enumerate(bid_depth[:10], 1):
                    bid_display.append({
                        "档位": f"买{i}",
                        "价格": f"¥{b['price']:.2f}",
                        "数量": b["qty"],
                        "累积": b["cumQty"],
                    })
                st.dataframe(
                    pd.DataFrame(bid_display),
                    width="stretch",
                    hide_index=True,
                )
            else:
                st.caption("无买盘")

        with book_col2:
            st.markdown("**🔴 卖盘 (Ask)**")
            if ask_depth:
                ask_display = []
                for i, a in enumerate(ask_depth[:10], 1):
                    ask_display.append({
                        "档位": f"卖{i}",
                        "价格": f"¥{a['price']:.2f}",
                        "数量": a["qty"],
                        "累积": a["cumQty"],
                    })
                st.dataframe(
                    pd.DataFrame(ask_display),
                    width="stretch",
                    hide_index=True,
                )
            else:
                st.caption("无卖盘")

        # ---- 成交记录 ----
        st.markdown("---")
        st.subheader("💹 最近成交")

        if trades:
            # 成交价格走势
            if len(trades) >= 2:
                price_df = pd.DataFrame(trades)
                price_df["序号"] = range(1, len(price_df) + 1)

                import altair as alt

                price_chart = alt.Chart(price_df).mark_line(
                    point=True, color="#6366f1"
                ).encode(
                    x=alt.X("序号:Q", title="成交序号"),
                    y=alt.Y("execPrice:Q", title="成交价 (¥)",
                            scale=alt.Scale(zero=False)),
                    tooltip=[
                        alt.Tooltip("execId:N", title="成交编号"),
                        alt.Tooltip("side:N", title="方向"),
                        alt.Tooltip("execQty:Q", title="数量"),
                        alt.Tooltip("execPrice:Q", title="价格", format=".2f"),
                    ],
                ).properties(
                    height=250, title="成交价格走势"
                ).interactive()

                st.altair_chart(price_chart, width="stretch")

            # 成交明细表
            trade_display = []
            for t in reversed(trades):  # 最新在前
                trade_display.append({
                    "成交编号": t["execId"],
                    "订单号": t["clOrderId"],
                    "证券": t["securityId"],
                    "方向": "🟢买" if t["side"] == "B" else "🔴卖",
                    "成交量": t["execQty"],
                    "成交价": f"¥{t['execPrice']:.2f}",
                    "股东号": t["shareholderId"],
                })
            st.dataframe(
                pd.DataFrame(trade_display),
                width="stretch",
                hide_index=True,
            )
        else:
            st.info("暂无成交记录")


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
                header_col1, header_col2, header_col3, header_col4 = st.columns([3, 1, 1, 0.5])
                with header_col1:
                    st.markdown(
                        f"**{side_emoji} {side_text} {order['securityId']}** "
                        f"({order['market']}) — `{order['clOrderId']}`"
                    )
                with header_col2:
                    if status == "已拒绝" and order.get("rejectReason"):
                        st.markdown(
                            f'<span title="{order["rejectReason"]}" style="cursor:help">'
                            f'<b>❌ 已拒绝</b></span>',
                            unsafe_allow_html=True,
                        )
                    else:
                        st.markdown(f":{color}[**{icon} {status}**]")
                with header_col3:
                    st.markdown(f"**{progress}%** 成交")
                with header_col4:
                    # 可撤单状态：已提交/已确认/部分成交
                    if status in ("已提交", "已确认", "部分成交"):
                        popover = st.popover("⋯")
                        with popover:
                            st.caption(f"订单 {order['clOrderId']}")
                            if st.button("🚫 撤单", key=f"cancel_{order['clOrderId']}",
                                         type="primary", use_container_width=True):
                                cancel_result = api_post(
                                    "/api/cancel",
                                    {
                                        "origClOrderId": order["clOrderId"],
                                        "market": order["market"],
                                        "securityId": order["securityId"],
                                        "shareholderId": order["shareholderId"],
                                        "side": order["side"],
                                    },
                                )
                                if cancel_result and cancel_result.get("status") == "submitted":
                                    st.success("撤单已提交")
                                    time.sleep(0.5)
                                    st.rerun()
                                else:
                                    st.error("撤单失败")

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


# ======================== 页面：交易所监控 ========================

elif page == "交易所监控":
    st.title("🏛️ 交易所监控")
    st.caption("实时监控模拟交易所（纯撮合系统）的状态和回报")

    # 刷新按钮
    if st.button("🔄 刷新"):
        st.rerun()

    # ---- 交易所统计概览 ----
    ex_status = api_get("/api/exchange/status")
    if ex_status:
        st.subheader("📊 交易所统计")
        col1, col2, col3, col4 = st.columns(4)
        col1.metric("总回报数", ex_status.get("totalResponses", 0))
        col2.metric("成交笔数", ex_status.get("executions", 0))
        col3.metric("确认回报", ex_status.get("confirms", 0))
        col4.metric("撤单确认", ex_status.get("cancels", 0))
    else:
        st.warning("无法获取交易所状态")

    st.markdown("---")

    # ---- 直接注入交易所订单 ----
    st.subheader("💉 注入交易所订单")
    st.caption("直接向交易所提交订单，模拟外部市场参与者（不经过前置风控）")

    with st.form("exchange_order_form"):
        ex_col1, ex_col2 = st.columns(2)
        with ex_col1:
            ex_market = st.selectbox("市场", ["XSHG", "XSHE", "BJSE"], key="ex_market")
            ex_security_id = st.text_input("证券代码", value="600030", key="ex_sec")
            ex_side = st.selectbox(
                "方向", ["B", "S"],
                format_func=lambda x: "买入" if x == "B" else "卖出",
                key="ex_side",
            )
        with ex_col2:
            ex_price = st.number_input("价格", min_value=0.01, value=10.0, step=0.01, key="ex_price")
            ex_qty = st.number_input("数量", min_value=1, value=100, step=100, key="ex_qty")
            ex_shareholder = st.text_input("股东号", value="EXT001", key="ex_sh")

        ex_submitted = st.form_submit_button("📤 注入交易所", type="primary")
        if ex_submitted:
            result = api_post(
                "/api/order",
                {
                    "market": ex_market,
                    "securityId": ex_security_id,
                    "side": ex_side,
                    "price": ex_price,
                    "qty": int(ex_qty),
                    "shareholderId": ex_shareholder,
                    "target": "exchange",
                },
            )
            if result and result.get("status") == "submitted":
                st.success(f"✅ 订单已直接注入交易所，编号: {result['clOrderId']}")
            else:
                st.error(f"❌ 注入失败: {result.get('message', '未知错误') if result else '连接失败'}")

    st.markdown("---")

    # ---- 交易所回报流水 ----
    st.subheader("📜 交易所回报流水")

    ex_responses = api_get("/api/exchange/responses", limit=100)
    if ex_responses and ex_responses.get("responses"):
        resps = ex_responses["responses"]

        # 分类展示
        exec_resps = [r for r in resps if "execId" in r]
        confirm_resps = [r for r in resps if "execId" not in r and "rejectCode" not in r and "origClOrderId" not in r]
        cancel_resps = [r for r in resps if "origClOrderId" in r and "rejectCode" not in r]
        reject_resps = [r for r in resps if "rejectCode" in r]

        tab1, tab2, tab3, tab4 = st.tabs([
            f"成交 ({len(exec_resps)})",
            f"确认 ({len(confirm_resps)})",
            f"撤单 ({len(cancel_resps)})",
            f"拒绝 ({len(reject_resps)})",
        ])

        with tab1:
            if exec_resps:
                exec_display = []
                for r in reversed(exec_resps):
                    exec_display.append({
                        "成交编号": r.get("execId", ""),
                        "订单号": r.get("clOrderId", ""),
                        "证券": r.get("securityId", ""),
                        "方向": "🟢买" if r.get("side") == "B" else "🔴卖",
                        "成交量": r.get("execQty", 0),
                        "成交价": f"¥{r.get('execPrice', 0):.2f}",
                        "股东号": r.get("shareholderId", ""),
                    })
                st.dataframe(pd.DataFrame(exec_display), width="stretch", hide_index=True)
            else:
                st.info("暂无成交记录")

        with tab2:
            if confirm_resps:
                confirm_display = []
                for r in reversed(confirm_resps):
                    confirm_display.append({
                        "订单号": r.get("clOrderId", ""),
                        "证券": r.get("securityId", ""),
                        "方向": "🟢买" if r.get("side") == "B" else "🔴卖",
                        "价格": f"¥{r.get('price', 0):.2f}" if r.get("price") else "—",
                        "数量": r.get("qty", 0),
                        "股东号": r.get("shareholderId", ""),
                    })
                st.dataframe(pd.DataFrame(confirm_display), width="stretch", hide_index=True)
            else:
                st.info("暂无确认回报")

        with tab3:
            if cancel_resps:
                st.json(cancel_resps)
            else:
                st.info("暂无撤单回报")

        with tab4:
            if reject_resps:
                st.json(reject_resps)
            else:
                st.info("暂无拒绝回报")
    else:
        st.info("暂无交易所回报，注入订单后将在此显示。")


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
                    st.dataframe(cross_trades, width="stretch")
                else:
                    st.info("无对敲拦截记录")
            with tab2:
                if format_errors:
                    st.dataframe(format_errors, width="stretch")
                else:
                    st.info("无格式错误记录")
            with tab3:
                if others:
                    st.dataframe(others, width="stretch")
                else:
                    st.info("无其他拒绝记录")
        else:
            st.info("暂无风控拦截记录")

    if st.button("🔄 刷新"):
        st.rerun()
