#include "../src/itch.hpp"

#include <random>

#include "../src/synth.hpp"
#include "../third_party/catch.hpp"

using namespace itch;

namespace {
bool bytes_equal(const Message& a, const Message& b) {
    uint8_t ba[MAX_MSG_LEN], bb[MAX_MSG_LEN];
    size_t na = encode(a, ba), nb = encode(b, bb);
    return na == nb && std::memcmp(ba, bb, na) == 0;
}
}  // namespace

TEST_CASE("codec: known sizes and layout basics") {
    AddOrder a;
    a.h.stock_locate = 7; a.h.tracking = 3; a.h.timestamp = 0x0000123456789ABCULL;
    a.order_ref = 42; a.side = 'B'; a.shares = 100;
    a.stock = {'A','A','P','L',' ',' ',' ',' '}; a.price = 1500000;

    uint8_t buf[MAX_MSG_LEN];
    REQUIRE(encode(Message{a}, buf) == LEN_A);
    CHECK(buf[0] == 'A');
    CHECK(get_u16(buf + 1) == 7);
    CHECK(get_u48(buf + 5) == 0x0000123456789ABCULL);
    CHECK(get_u64(buf + 11) == 42);
    CHECK(buf[19] == 'B');
    CHECK(get_u32(buf + 32) == 1500000);

    auto d = decode(buf, LEN_A);
    REQUIRE(d.has_value());
    CHECK(bytes_equal(*d, Message{a}));
}

TEST_CASE("codec: encode(decode(x)) == x across all 7 types, randomized") {
    std::mt19937_64 rng(2026);
    auto r64 = [&] { return rng(); };
    auto r32 = [&] { return uint32_t(rng()); };
    auto r16 = [&] { return uint16_t(rng()); };
    auto rhdr = [&] {
        Header h;
        h.stock_locate = r16(); h.tracking = r16();
        h.timestamp = r64() & 0xFFFFFFFFFFFFULL;  // 48-bit field
        return h;
    };

    for (int i = 0; i < 20000; ++i) {
        Message m;
        switch (i % 7) {
            case 0: { AddOrder x; x.h = rhdr(); x.order_ref = r64();
                      x.side = (i & 8) ? 'B' : 'S'; x.shares = r32();
                      x.stock = {'Z','V','Z','Z','T',' ',' ',' '};
                      x.price = r32(); m = x; break; }
            case 1: { AddOrderMpid x; x.add.h = rhdr(); x.add.order_ref = r64();
                      x.add.side = 'S'; x.add.shares = r32();
                      x.add.stock = {'Q','Q','Q',' ',' ',' ',' ',' '};
                      x.add.price = r32();
                      x.attribution = {'L','E','H','M'}; m = x; break; }
            case 2: { OrderExecuted x; x.h = rhdr(); x.order_ref = r64();
                      x.shares = r32(); x.match_num = r64(); m = x; break; }
            case 3: { OrderExecutedPrice x; x.exec.h = rhdr();
                      x.exec.order_ref = r64(); x.exec.shares = r32();
                      x.exec.match_num = r64();
                      x.printable = (i & 8) ? 'Y' : 'N';
                      x.price = r32(); m = x; break; }
            case 4: { OrderCancel x; x.h = rhdr(); x.order_ref = r64();
                      x.shares = r32(); m = x; break; }
            case 5: { OrderDelete x; x.h = rhdr(); x.order_ref = r64();
                      m = x; break; }
            default:{ OrderReplace x; x.h = rhdr(); x.orig_order_ref = r64();
                      x.new_order_ref = r64(); x.shares = r32();
                      x.price = r32(); m = x; break; }
        }
        uint8_t buf[MAX_MSG_LEN];
        size_t n = encode(m, buf);
        auto d = decode(buf, n);
        REQUIRE(d.has_value());
        CHECK(type_of(*d) == type_of(m));
        CHECK(bytes_equal(*d, m));
    }
}

TEST_CASE("codec: malformed input is rejected, never crashes") {
    uint8_t buf[MAX_MSG_LEN];
    AddOrder a; a.side = 'B'; a.stock = {'X',' ',' ',' ',' ',' ',' ',' '};
    size_t n = encode(Message{a}, buf);

    CHECK(!decode(buf, 0).has_value());
    CHECK(!decode(buf, n - 1).has_value());       // truncated
    CHECK(!decode(buf, n + 1).has_value());       // wrong length for type
    buf[0] = 'Z';
    CHECK(!decode(buf, n).has_value());           // unknown type
    buf[0] = 'A'; buf[19] = 'Q';
    CHECK(!decode(buf, n).has_value());           // bad side enum

    // Bit-flip fuzz: decode must never crash; if it succeeds the re-encoding
    // must be byte-identical (i.e., every accepted byte pattern round-trips).
    std::mt19937_64 rng(7);
    for (int i = 0; i < 50000; ++i) {
        uint8_t junk[MAX_MSG_LEN + 4];
        size_t len = rng() % (MAX_MSG_LEN + 4);
        for (size_t j = 0; j < len; ++j) junk[j] = uint8_t(rng());
        auto d = decode(junk, len);
        if (d) {
            uint8_t out[MAX_MSG_LEN];
            size_t m = encode(*d, out);
            REQUIRE(m == len);
            CHECK(std::memcmp(out, junk, m) == 0);
        }
    }
}

TEST_CASE("codec: framed stream round-trip via FrameReader") {
    synth::Generator gen({.seed = 99});
    std::vector<Message> sent;
    std::vector<uint8_t> wire;
    bool ok;
    for (int i = 0; i < 5000; ++i) {
        Message m = gen.next(ok);
        sent.push_back(m);
        encode_framed(m, wire);
    }
    FrameReader rd{wire.data(), wire.size()};
    size_t i = 0;
    while (auto m = rd.next()) {
        REQUIRE(i < sent.size());
        CHECK(bytes_equal(*m, sent[i]));
        ++i;
    }
    CHECK(!rd.error);
    CHECK(i == sent.size());

    // Truncated stream flags an error instead of looping or crashing.
    FrameReader bad{wire.data(), wire.size() - 3};
    while (bad.next()) {}
    CHECK(bad.error);
}

namespace {
// Append one frame with an arbitrary body (for unknown-type / malformed
// stream construction).
void frame_raw(std::vector<uint8_t>& wire, const uint8_t* body, uint16_t n) {
    size_t at = wire.size();
    wire.resize(at + 2 + n);
    put_u16(wire.data() + at, n);
    std::memcpy(wire.data() + at + 2, body, n);
}
}  // namespace

TEST_CASE("framing: unknown types are skipped and counted, not fatal") {
    AddOrder a; a.side = 'B'; a.shares = 100; a.price = 1000;
    a.order_ref = 1; a.stock = {'X',' ',' ',' ',' ',' ',' ',' '};
    OrderDelete d; d.order_ref = 1;

    std::vector<uint8_t> wire;
    uint8_t sys[12] = {'S', 0,0, 0,0, 0,0,0,0,0,0, 'O'};   // system event
    uint8_t trade[44] = {'P'};                              // non-cross trade
    uint8_t dir[39] = {'R'};                                // stock directory
    frame_raw(wire, sys, sizeof sys);
    frame_raw(wire, dir, sizeof dir);
    encode_framed(Message{a}, wire);
    frame_raw(wire, trade, sizeof trade);
    frame_raw(wire, sys, sizeof sys);
    encode_framed(Message{d}, wire);

    FrameReader rd{wire.data(), wire.size()};
    std::vector<char> seen;
    while (auto m = rd.next()) seen.push_back(type_of(*m));
    CHECK(!rd.error);
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == 'A');
    CHECK(seen[1] == 'D');
    CHECK(rd.skipped == 4);
    CHECK(rd.skips[uint8_t('S')] == 2);
    CHECK(rd.skips[uint8_t('P')] == 1);
    CHECK(rd.skips[uint8_t('R')] == 1);
}

TEST_CASE("framing: malformed frames stay fatal, never skipped") {
    AddOrder a; a.side = 'B'; a.shares = 100; a.price = 1000;
    a.order_ref = 1; a.stock = {'X',' ',' ',' ',' ',' ',' ',' '};

    SECTION("zero-length frame") {
        std::vector<uint8_t> wire;
        encode_framed(Message{a}, wire);
        wire.push_back(0); wire.push_back(0);          // len = 0
        encode_framed(Message{a}, wire);
        FrameReader rd{wire.data(), wire.size()};
        CHECK(rd.next().has_value());                  // first A decodes
        CHECK(!rd.next().has_value());                 // then hard stop
        CHECK(rd.error);
    }
    SECTION("unknown type whose length runs past the buffer") {
        std::vector<uint8_t> wire;
        uint8_t junk[4] = {'Q', 1, 2, 3};
        frame_raw(wire, junk, sizeof junk);
        wire[0] = 0xFF; wire[1] = 0xFF;                // lie about length
        FrameReader rd{wire.data(), wire.size()};
        CHECK(!rd.next().has_value());
        CHECK(rd.error);
        CHECK(rd.skipped == 0);
    }
    SECTION("known type with wrong length") {
        std::vector<uint8_t> wire;
        encode_framed(Message{a}, wire);
        // Shrink the A frame's declared length: still in-buffer, wrong size.
        put_u16(wire.data(), LEN_A - 1);
        wire.resize(2 + LEN_A - 1);
        FrameReader rd{wire.data(), wire.size()};
        CHECK(!rd.next().has_value());
        CHECK(rd.error);
    }
    SECTION("known type with bad enum value") {
        std::vector<uint8_t> wire;
        encode_framed(Message{a}, wire);
        wire[2 + 19] = 'Q';                            // side neither B nor S
        FrameReader rd{wire.data(), wire.size()};
        CHECK(!rd.next().has_value());
        CHECK(rd.error);
    }
}

TEST_CASE("stock directory: parse the real-world 'R' layout") {
    // Byte-for-byte the second frame of data/20190730.BX_ITCH_50 (verified
    // by hand against the file): locate 1, stock "A", round lot 100.
    const uint8_t r[39] = {
        'R',  0x00,0x01, 0x00,0x00, 0x0a,0x65,0xaf,0x1f,0xc0,0xcf,
        'A',' ',' ',' ',' ',' ',' ',' ',
        'N', ' ', 0x00,0x00,0x00,0x64, 'N', 'C',
        'Z',' ', 'P', 'N', ' ', '1', 'N', 0x00,0x00,0x00,0x00, 'N'};
    auto d = parse_stock_directory(r, sizeof r);
    REQUIRE(d.has_value());
    CHECK(d->h.stock_locate == 1);
    CHECK(std::string(d->stock.data(), 8) == "A       ");
    CHECK(d->market_category == 'N');
    CHECK(d->financial_status == ' ');
    CHECK(d->round_lot == 100);
    CHECK(d->round_lots_only == 'N');
    CHECK(d->issue_class == 'C');

    CHECK(!parse_stock_directory(r, 38).has_value());
    uint8_t wrong[39]; std::memcpy(wrong, r, 39); wrong[0] = 'A';
    CHECK(!parse_stock_directory(wrong, 39).has_value());
}
