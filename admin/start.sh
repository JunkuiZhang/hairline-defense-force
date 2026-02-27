#!/bin/bash
# start.sh — 一键启动管理界面（FastAPI + Streamlit）
#
# 使用方法：
#   chmod +x start.sh
#   ./start.sh
#
# 前提：
#   1. conda 环境 hdf 已创建且已安装依赖
#   2. C++ 程序（含 AdminServer）已启动并监听 9900 端口

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=========================================="
echo "        发际线保卫队 撮合系统管理界面"
echo "=========================================="

echo "[1/2] 启动 FastAPI 后端 (port 8000)..."
cd "$SCRIPT_DIR"
uvicorn server:app --host 127.0.0.1 --port 8000 &
FASTAPI_PID=$!
echo "      PID: $FASTAPI_PID"

sleep 1

echo "[2/2] 启动 Streamlit 前端 (port 8501)..."
streamlit run app.py --server.port 8501 --server.headless true &
STREAMLIT_PID=$!
echo "      PID: $STREAMLIT_PID"

echo ""
echo "=========================================="
echo "  管理界面已启动："
echo "    前端: http://localhost:8501"
echo "    API:  http://localhost:8000/docs"
echo "=========================================="
echo "  按 Ctrl+C 停止所有服务"
echo "=========================================="

# 捕获 Ctrl+C，停止所有进程
cleanup() {
    echo ""
    echo "Stopping services..."
    kill $FASTAPI_PID 2>/dev/null || true
    kill $STREAMLIT_PID 2>/dev/null || true
    echo "Done."
}
trap cleanup EXIT INT TERM

# 等待任一进程退出
wait
