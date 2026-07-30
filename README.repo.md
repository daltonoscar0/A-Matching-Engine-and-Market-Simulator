# exchange

C++17 limit order book + ITCH 5.0 codec (Phase 1 of the Exchange project).

## Build & test
    cmake -B build && cmake --build build -j && (cd build && ctest)
    FUZZ_N=1000000 ./build/tests "fuzz*"     # full 1M-message fuzz gate

## Bench
    ./build/bench_gen /tmp/synth.itch 5000000 42
    ./build/bench_replay /tmp/synth.itch 3

## Layout
    src/itch.{hpp,cpp}   ITCH 5.0 subset (A F E C X D U) + framed codec
    src/book.{hpp,cpp}   price-time priority book, invariant ledger + audit
    src/feed.hpp         ITCH message -> book operation adapter
    src/synth.hpp        synthetic stream generator (LOBSTER stand-in)
    tests/               Catch2 property tests + randomized fuzzer
    bench/               stream generator + replay benchmark
