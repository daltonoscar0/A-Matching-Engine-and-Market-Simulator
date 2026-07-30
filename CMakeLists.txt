cmake_minimum_required(VERSION 3.16)
project(exchange CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")
add_compile_options(-Wall -Wextra)

add_library(exchange_core src/itch.cpp src/book.cpp)
target_include_directories(exchange_core PUBLIC src)

# Tests (Catch2 v2.13.10 vendored as a single header in third_party/)
add_executable(tests
  tests/main.cpp tests/test_book.cpp tests/test_codec.cpp tests/test_fuzz.cpp)
target_link_libraries(tests PRIVATE exchange_core)

enable_testing()
add_test(NAME unit_and_fuzz COMMAND tests)

# Bench
add_executable(bench_gen bench/gen.cpp)
target_link_libraries(bench_gen PRIVATE exchange_core)
add_executable(bench_replay bench/replay.cpp)
target_link_libraries(bench_replay PRIVATE exchange_core)
