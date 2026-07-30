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
  tests/main.cpp tests/test_book.cpp tests/test_bookset.cpp
  tests/test_codec.cpp tests/test_fuzz.cpp tests/test_itch_replay.cpp
  tests/test_match.cpp)
target_link_libraries(tests PRIVATE exchange_core)

enable_testing()
add_test(NAME unit_and_fuzz COMMAND tests)

# Bench
add_executable(bench_gen bench/gen.cpp)
target_link_libraries(bench_gen PRIVATE exchange_core)
add_executable(bench_replay bench/replay.cpp)
target_link_libraries(bench_replay PRIVATE exchange_core)
add_executable(bench_tail bench/tail.cpp)
target_link_libraries(bench_tail PRIVATE exchange_core)
add_executable(bench_match bench/match.cpp)
target_link_libraries(bench_match PRIVATE exchange_core)
add_executable(replay_itch bench/replay_itch.cpp)
target_link_libraries(replay_itch PRIVATE exchange_core)

# Tools (verification / analysis over the real data day)
add_executable(field_sanity tools/field_sanity.cpp)
target_link_libraries(field_sanity PRIVATE exchange_core)
add_executable(bbo_trace tools/bbo_trace.cpp)
target_link_libraries(bbo_trace PRIVATE exchange_core)
add_executable(stylized tools/stylized.cpp)
target_link_libraries(stylized PRIVATE exchange_core)
add_executable(gap_hist tools/gap_hist.cpp)
target_link_libraries(gap_hist PRIVATE exchange_core)
