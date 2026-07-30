#include "../src/book.hpp"

#include "../third_party/catch.hpp"

using lob::Book;
using lob::Result;
using lob::Side;

TEST_CASE("empty book basics") {
    Book b;
    CHECK(b.best_bid() == 0);
    CHECK(b.best_ask() == 0);
    CHECK(!b.crossed());
    CHECK(b.audit().empty());
    CHECK(b.remove(1) == Result::UnknownId);
    CHECK(b.execute(1, 100) == Result::UnknownId);
    CHECK(b.cancel(1, 100) == Result::UnknownId);
}

TEST_CASE("property: bids never cross asks") {
    Book b;
    REQUIRE(b.add(1, Side::Buy, 100, 999900) == Result::Ok);   // 99.99 bid
    REQUIRE(b.add(2, Side::Sell, 100, 1000100) == Result::Ok); // 100.01 ask

    SECTION("crossing and locking adds are rejected") {
        CHECK(b.add(3, Side::Buy, 100, 1000100) == Result::WouldCross); // = ask
        CHECK(b.add(4, Side::Buy, 100, 1000200) == Result::WouldCross); // > ask
        CHECK(b.add(5, Side::Sell, 100, 999900) == Result::WouldCross); // = bid
        CHECK(b.add(6, Side::Sell, 100, 999800) == Result::WouldCross); // < bid
        CHECK(!b.crossed());
        CHECK(b.open_orders() == 2);
    }
    SECTION("adds inside the spread are fine") {
        CHECK(b.add(3, Side::Buy, 100, 1000000) == Result::Ok);
        CHECK(b.add(4, Side::Sell, 100, 1000050) == Result::Ok);
        CHECK(!b.crossed());
        CHECK(b.best_bid() == 1000000);
        CHECK(b.best_ask() == 1000050);
    }
    SECTION("replace cannot create a cross") {
        CHECK(b.replace(1, 10, 100, 1000100) == Result::WouldCross);
        CHECK(b.find(1) != nullptr);         // rejected replace is a no-op
        CHECK(b.find(10) == nullptr);
        CHECK(b.audit().empty());
    }
    CHECK(b.audit().empty());
}

TEST_CASE("property: FIFO within a price level") {
    Book b;
    REQUIRE(b.add(1, Side::Buy, 100, 1000000) == Result::Ok);
    REQUIRE(b.add(2, Side::Buy, 200, 1000000) == Result::Ok);
    REQUIRE(b.add(3, Side::Buy, 300, 1000000) == Result::Ok);

    // Queue order is 1,2,3 by arrival.
    const lob::Order* o1 = b.find(1);
    REQUIRE(o1);
    CHECK(o1->prev == nullptr);              // 1 is at the front
    CHECK(o1->next == b.find(2));
    CHECK(b.find(2)->next == b.find(3));
    CHECK(b.find(3)->next == nullptr);

    SECTION("deleting the middle preserves relative order") {
        REQUIRE(b.remove(2) == Result::Ok);
        CHECK(b.find(1)->next == b.find(3));
        CHECK(b.find(3)->prev == b.find(1));
        CHECK(b.audit().empty());
    }
    SECTION("replace loses time priority (goes to the back)") {
        REQUIRE(b.replace(1, 9, 100, 1000000) == Result::Ok);
        const lob::Order* o9 = b.find(9);
        REQUIRE(o9);
        CHECK(o9->next == nullptr);          // 9 is now last
        CHECK(o9->prev == b.find(3));
        CHECK(b.find(2)->prev == nullptr);   // 2 moved to the front
        CHECK(b.audit().empty());
    }
    SECTION("partial execute does not reorder the queue") {
        REQUIRE(b.execute(1, 50) == Result::Ok);
        CHECK(b.find(1)->prev == nullptr);   // still first
        CHECK(b.find(1)->shares == 50);
        CHECK(b.audit().empty());
    }
    SECTION("full execute removes from the front cleanly") {
        REQUIRE(b.execute(1, 100) == Result::Ok);
        CHECK(b.find(1) == nullptr);
        CHECK(b.find(2)->prev == nullptr);
        CHECK(b.audit().empty());
    }
}

TEST_CASE("property: share conservation across add/execute/cancel") {
    Book b;
    REQUIRE(b.add(1, Side::Buy, 1000, 1000000) == Result::Ok);
    REQUIRE(b.add(2, Side::Sell, 500, 1000100) == Result::Ok);
    REQUIRE(b.execute(1, 400) == Result::Ok);
    REQUIRE(b.cancel(1, 100) == Result::Ok);
    REQUIRE(b.remove(2) == Result::Ok);

    CHECK(b.shares_added() == 1500);
    CHECK(b.shares_executed() == 400);
    CHECK(b.shares_canceled() == 600);       // 100 partial + 500 delete
    CHECK(b.shares_resting() == 500);
    CHECK(b.shares_added() ==
          b.shares_resting() + b.shares_executed() + b.shares_canceled());
    CHECK(b.invariants_fast());
    CHECK(b.audit().empty());

    SECTION("replace books remainder as canceled, new shares as added") {
        REQUIRE(b.replace(1, 3, 700, 999900) == Result::Ok);
        CHECK(b.shares_canceled() == 600 + 500);   // orig remainder canceled
        CHECK(b.shares_added() == 1500 + 700);
        CHECK(b.shares_resting() == 700);
        CHECK(b.invariants_fast());
        CHECK(b.audit().empty());
    }
}

TEST_CASE("property: operations on unknown/duplicate ids are rejected") {
    Book b;
    REQUIRE(b.add(1, Side::Buy, 100, 1000000) == Result::Ok);

    CHECK(b.add(1, Side::Sell, 100, 2000000) == Result::DuplicateId);
    CHECK(b.execute(99, 10) == Result::UnknownId);
    CHECK(b.cancel(99, 10) == Result::UnknownId);
    CHECK(b.remove(99) == Result::UnknownId);
    CHECK(b.replace(99, 100, 50, 1000000) == Result::UnknownId);
    CHECK(b.replace(1, 1, 50, 1000000) == Result::BadReplace);

    // New id colliding with a live order.
    REQUIRE(b.add(2, Side::Buy, 100, 999900) == Result::Ok);
    CHECK(b.replace(1, 2, 50, 1000000) == Result::DuplicateId);

    // Rejections must leave the book untouched.
    CHECK(b.open_orders() == 2);
    CHECK(b.find(1)->shares == 100);
    CHECK(b.audit().empty());
}

TEST_CASE("edge: over-execute and over-cancel rejected, exact drain ok") {
    Book b;
    REQUIRE(b.add(1, Side::Sell, 100, 1000000) == Result::Ok);
    CHECK(b.execute(1, 101) == Result::TooManyShares);
    CHECK(b.cancel(1, 101) == Result::TooManyShares);
    CHECK(b.execute(1, 0) == Result::ZeroShares);
    CHECK(b.find(1)->shares == 100);
    CHECK(b.execute(1, 100) == Result::Ok);  // exact drain deletes the order
    CHECK(b.find(1) == nullptr);
    CHECK(b.best_ask() == 0);                // level cleaned up
    CHECK(b.audit().empty());
}

TEST_CASE("edge: level teardown and rebuild") {
    Book b;
    REQUIRE(b.add(1, Side::Buy, 100, 1000000) == Result::Ok);
    REQUIRE(b.add(2, Side::Buy, 100, 1000000) == Result::Ok);
    REQUIRE(b.remove(1) == Result::Ok);
    REQUIRE(b.remove(2) == Result::Ok);
    CHECK(b.best_bid() == 0);
    REQUIRE(b.add(3, Side::Buy, 100, 1000000) == Result::Ok);  // same price again
    CHECK(b.best_bid() == 1000000);
    CHECK(b.find(3)->prev == nullptr);
    CHECK(b.audit().empty());
}
