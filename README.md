# 发际线保卫队

模拟股票交易中的撮合与风控系统。系统可作为**纯撮合交易所**独立运行，也可作为**交易所前置**在客户端与交易所之间进行内部撮合和对敲检测。

---

## 项目结构

```
├── include/                       # 头文件
│   ├── types.h                    # 数据结构（Order, CancelOrder, MarketData 等）
│   ├── constants.h                # 错误码常量
│   ├── matching_engine.h          # 撮合引擎接口
│   ├── risk_controller.h          # 风控引擎接口
│   ├── security_core.h            # 单证券核心业务单元（风控+撮合+状态管理）
│   ├── trade_system.h             # 交易系统主控接口（多 Bucket 并行架构）
│   ├── trade_logger.h             # 交易日志记录器接口
│   └── admin_server.h             # 管理后台 HTTP 服务接口
├── src/                           # 实现
│   ├── matching_engine.cpp        # 撮合引擎实现
│   ├── risk_controller.cpp        # 风控引擎实现
│   ├── security_core.cpp          # 单证券核心业务单元实现
│   ├── trade_system.cpp           # 交易系统主控实现（路由、WorkerBucket、MPSC 队列）
│   ├── trade_logger.cpp           # 交易日志记录器实现
│   └── admin_server.cpp           # 管理后台 HTTP 服务实现
├── tests/                         # 单元测试
│   ├── json_test.cpp              # JSON 解析 / 枚举转换测试
│   ├── matching_test.cpp          # 撮合引擎测试
│   ├── risk_test.cpp              # 风控引擎测试
│   ├── exchange_test.cc           # 纯撮合模式集成测试
│   ├── gateway_test.cc            # 交易所前置模式集成测试
│   ├── requirement_test.cpp       # 项目书要求一一对应测试
│   ├── trade_logger_test.cpp      # 交易日志测试
│   └── example_test.cc            # 示例测试
├── benchmarks/                    # 性能基准测试
│   ├── benchmark.cpp              # 单线程全链路吞吐量 / 延时测试
│   ├── bench_matching.cpp         # 撮合引擎裸性能专项测试
│   ├── bench_concurrent.cpp       # 并发扩展性测试
│   ├── bench_multicore.cpp        # 多 Bucket 并行扩展性测试
│   └── bench_network.cpp          # 网络性能测试
├── examples/                      # 示例程序
│   ├── exchange.cpp               # 纯撮合模式示例
│   ├── pre_exchange.cpp           # 交易所前置模式示例
│   ├── admin_main.cpp             # 管理后台示例（HTTP API + WebSocket）
│   └── generate_history.cpp       # 历史数据生成工具
├── admin/                         # 管理后台前端（Python）
│   ├── app.py                     # Streamlit 前端
│   ├── server.py                  # FastAPI 后端
│   ├── bridge.py                  # C++ 后端桥接
│   └── protocol.py                # 通信字段定义
├── docs/                          # 文档
│   ├── task_breakdown.md          # 项目分工表
│   ├── how_to_contribute.md       # 贡献指南
│   └── benchmarks/                # 性能测试记录
└── CMakeLists.txt
```

## 文档

- [**项目分工表**](docs/task_breakdown.md) — 任务列表、认领表
- [**贡献指南**](docs/how_to_contribute.md) — 如何参与开发

## 编译与运行

### 环境要求

目前我只在Linux上进行过测试，
构建时需要ninja构建工具，Ubuntu系统可以通过以下命令安装：

```bash
sudo apt install ninja-build
```

### 运行测试

```bash
cmake --build build --target unit_tests
./bin/unit_tests
```

### 运行示例

```bash
# 纯撮合模式
cmake --build build --target exchange
./bin/exchange

# 交易所前置模式
cmake --build build --target pre_exchange
./bin/pre_exchange
```