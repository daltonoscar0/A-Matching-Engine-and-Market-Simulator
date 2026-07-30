// BookSet routing: messages land in the book named by stock_locate, and
// books are fully isolated from each other.
#include "../src/bookset.hpp"
#include "../src/itch.hpp"
#include "../third_party/catch.hpp"

namespace {

itch::AddOrder add(uint16_t locate, uint64_t ref, char side, uint32_t shares,
                   uint32_t price) {
    itch::AddOrder a;
    a.h.stock_locate = locate;
    a.order_ref = ref; a.side = side; a.shares = shares; a.price = price;
    return a;
}

}  // namespace

TEST_CASE("bookset: routes adds by stock_locate") {
    lob::BookSet set;
    REQUIRE(set.apply(add(1, 101, 'B', 100, 1'000'000)) == lob::Result::Ok);
    REQUIRE(set.apply(add(2, 201, 'S', 200, 900'000)) == lob::Result::Ok);

    REQUIRE(set.book_count() == 2);
    REQUIRE(set.find(1) != nullptr);
    REQUIRE(set.find(2) != nullptr);
    REQUIRE(set.find(3) == nullptr);

    CHECK(set.book(1).best_bid() == 1'000'000);
    CHECK(set.book(1).best_ask() == 0);
    CHECK(set.book(2).best_ask() == 900'000);
    CHECK(set.book(2).best_bid() == 0);
    CHECK(set.book(1).find(101) != nullptr);
    CHECK(set.book(1).find(201) == nullptr);
    CHECK(set.book(2).find(201) != nullptr);
}

TEST_CASE("bookset: books are isolated - no cross-symbol crossing") {
    // Instrument 2's ask at 900'000 sits below instrument 1's bid at
    // 1'000'000. In one book that add would be rejected as crossing; in a
    // BookSet the two must not interact.
    lob::BookSet set;
    REQUIRE(set.apply(add(1, 1, 'B', 100, 1'000'000)) == lob::Result::Ok);
    REQUIRE(set.apply(add(2, 2, 'S', 100, 900'000)) == lob::Result::Ok);
    CHECK(!set.book(1).crossed());
    CHECK(!set.book(2).crossed());

    // Same book still rejects a crossing add.
    CHECK(set.apply(add(2, 3, 'B', 100, 900'000)) == lob::Result::WouldCross);
}

TEST_CASE("bookset: execute/cancel/delete/replace follow the locate code") {
    lob::BookSet set;
    REQUIRE(set.apply(add(7, 70, 'B', 300, 500'000)) == lob::Result::Ok);

    itch::OrderExecuted e;
    e.h.stock_locate = 7; e.order_ref = 70; e.shares = 100; e.match_num = 1;
    REQUIRE(set.apply(e) == lob::Result::Ok);
    CHECK(set.book(7).shares_executed() == 100);

    // Same ref via the wrong locate: routed to a different (fresh) book,
    // where the ref is unknown.
    e.h.stock_locate = 8;
    CHECK(set.apply(e) == lob::Result::UnknownId);
    CHECK(set.book(7).find(70)->shares == 200);

    itch::OrderReplace u;
    u.h.stock_locate = 7; u.orig_order_ref = 70; u.new_order_ref = 71;
    u.shares = 50; u.price = 490'000;
    REQUIRE(set.apply(u) == lob::Result::Ok);
    CHECK(set.book(7).find(70) == nullptr);
    CHECK(set.book(7).find(71)->price == 490'000);

    itch::OrderDelete d;
    d.h.stock_locate = 7; d.order_ref = 71;
    REQUIRE(set.apply(d) == lob::Result::Ok);
    CHECK(set.book(7).open_orders() == 0);
}

TEST_CASE("bookset: locate_of covers every message type") {
    itch::Header h; h.stock_locate = 42;
    itch::AddOrder a; a.h = h;
    itch::AddOrderMpid f; f.add.h = h;
    itch::OrderExecuted e; e.h = h;
    itch::OrderExecutedPrice c; c.exec.h = h;
    itch::OrderCancel x; x.h = h;
    itch::OrderDelete d; d.h = h;
    itch::OrderReplace u; u.h = h;
    for (itch::Message m : {itch::Message(a), itch::Message(f),
                            itch::Message(e), itch::Message(c),
                            itch::Message(x), itch::Message(d),
                            itch::Message(u)})
        CHECK(lob::locate_of(m) == 42);
}
