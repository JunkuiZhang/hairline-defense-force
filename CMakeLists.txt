cmake_minimum_required(VERSION 3.28)

set(CMAKE_CXX_STANDARD 23)

project(hairline_defense_force)

include_directories(include)
include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/lib)

# Library with core logic
add_library(trade_engine
  src/risk_controller.cpp
  src/matching_engine.cpp
  src/trade_system.cpp
)
target_link_libraries(trade_engine nlohmann_json::nlohmann_json)
target_include_directories(trade_engine PUBLIC include)

# Examples
# exchange.cpp演示了一个纯撮合系统的实现，用户端发送订单指令，系统处理后直接输出结果，不与交易所交互。
add_executable(exchange examples/exchange.cpp)
target_link_libraries(exchange trade_engine)
# pre-exchange.cpp演示了一个预交易系统的实现，用户端发送订单指令，系统进行风险控制和撮合处理后，再将结果发送到交易所。
add_executable(pre_exchange examples/pre_exchange.cpp)
target_link_libraries(pre_exchange trade_engine)

# Tests
enable_testing()
add_executable(unit_tests 
  tests/example_test.cc
  tests/risk_test.cpp
  tests/matching_test.cpp
)
target_link_libraries(unit_tests gtest_main trade_engine)

include(GoogleTest)
gtest_discover_tests(unit_tests)
