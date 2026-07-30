// bench_gen - write a framed synthetic ITCH stream to a file.
// Usage: bench_gen <out_file> [n_messages=5000000] [seed=42]
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/itch.hpp"
#include "../src/synth.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <out_file> [n=5000000] [seed=42]\n",
                     argv[0]);
        return 2;
    }
    size_t   n    = argc > 2 ? strtoull(argv[2], nullptr, 10) : 5'000'000;
    uint64_t seed = argc > 3 ? strtoull(argv[3], nullptr, 10) : 42;

    synth::Generator gen({.seed = seed});
    std::vector<uint8_t> wire;
    wire.reserve(n * 40);
    size_t counts[7] = {};  // A F E C X D U
    bool ok;
    for (size_t i = 0; i < n; ++i) {
        itch::Message m = gen.next(ok);
        itch::encode_framed(m, wire);
        switch (itch::type_of(m)) {
            case 'A': counts[0]++; break; case 'F': counts[1]++; break;
            case 'E': counts[2]++; break; case 'C': counts[3]++; break;
            case 'X': counts[4]++; break; case 'D': counts[5]++; break;
            case 'U': counts[6]++; break;
        }
    }

    FILE* f = std::fopen(argv[1], "wb");
    if (!f) { std::perror("fopen"); return 1; }
    if (std::fwrite(wire.data(), 1, wire.size(), f) != wire.size()) {
        std::perror("fwrite"); return 1;
    }
    std::fclose(f);

    std::printf("wrote %zu messages (%.1f MB) to %s\n"
                "mix: A=%zu F=%zu E=%zu C=%zu X=%zu D=%zu U=%zu\n"
                "end state: open=%zu best_bid=%u best_ask=%u\n",
                n, wire.size() / 1e6, argv[1],
                counts[0], counts[1], counts[2], counts[3], counts[4],
                counts[5], counts[6],
                gen.shadow().open_orders(), gen.shadow().best_bid(),
                gen.shadow().best_ask());
    return 0;
}
