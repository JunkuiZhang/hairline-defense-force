# 发际线保卫队

模拟股票交易中的撮合与风控系统。系统可作为**纯撮合交易所**独立运行，也可作为**交易所前置**在客户端与交易所之间进行内部撮合和对敲检测。

---

## 项目结构

```
├── include/                  # 头文件
│   ├── types.h               # 数据结构
│   ├── constants.h            # 错误码常量
│   ├── matching_engine.h      # 撮合引擎接口
│   ├── risk_controller.h      # 风控引擎接口
│   └── trade_system.h         # 交易系统主控接口
├── src/                      # 实现
│   ├── matching_engine.cpp    # 撮合引擎实现
│   ├── risk_controller.cpp    # 风控引擎实现
│   └── trade_system.cpp       # 交易系统主控实现
├── tests/                    # 单元测试
│   ├── json_test.cpp          # JSON 解析 / 枚举转换测试
│   ├── matching_test.cpp      # 撮合引擎测试
│   ├── risk_test.cpp          # 风控引擎测试
│   └── example_test.cc        # 示例测试
├── examples/                 # 示例程序
│   ├── exchange.cpp           # 纯撮合模式示例
│   ├── pre_exchange.cpp       # 交易所前置模式示例
│   └── demo_input.jsonl       # 示例输入数据
├── docs/                     # 文档
│   ├── task_breakdown.md      # 项目分工表
│   └── how_to_contribute.md   # 贡献指南
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