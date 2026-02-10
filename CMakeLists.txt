cmake_minimum_required(VERSION 3.28)

set(CMAKE_CXX_STANDARD 23)

project(group-9)

include_directories(include)
include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(googletest)

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/lib)

add_executable(basic_example examples/hello_world.cc)

add_executable(basic_test tests/example_test.cc)
target_link_libraries(basic_test gtest_main)
