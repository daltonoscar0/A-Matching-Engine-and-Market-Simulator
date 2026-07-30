// bookset.hpp - route ITCH messages to per-instrument Books by stock_locate.
//
// Storage is a vector indexed directly by locate (16-bit key space, so the
// worst case is 64k pointers = 512KB): O(1) routing with no hashing. Books
// are created on first touch. The single-book path (feed.hpp apply on a
// Book&) is untouched; BookSet is a layer above it.
#pragma once
#include <memory>
#include <vector>

#include "book.hpp"
#include "feed.hpp"
#include "itch.hpp"

namespace lob {

inline uint16_t locate_of(const itch::Message& m) {
    struct V {
        uint16_t operator()(const itch::AddOrder& x) const           { return x.h.stock_locate; }
        uint16_t operator()(const itch::AddOrderMpid& x) const       { return x.add.h.stock_locate; }
        uint16_t operator()(const itch::OrderExecuted& x) const      { return x.h.stock_locate; }
        uint16_t operator()(const itch::OrderExecutedPrice& x) const { return x.exec.h.stock_locate; }
        uint16_t operator()(const itch::OrderCancel& x) const        { return x.h.stock_locate; }
        uint16_t operator()(const itch::OrderDelete& x) const        { return x.h.stock_locate; }
        uint16_t operator()(const itch::OrderReplace& x) const       { return x.h.stock_locate; }
    };
    return std::visit(V{}, m);
}

class BookSet {
public:
    // reserve_per_book != 0 pre-sizes each new book's order pool (see
    // Book::reserve; per-book, so keep it proportional to expected depth).
    explicit BookSet(size_t reserve_per_book = 0)
        : reserve_(reserve_per_book) {}

    // Get-or-create the book for a locate code.
    Book& book(uint16_t locate) {
        if (locate >= books_.size()) books_.resize(size_t(locate) + 1);
        auto& slot = books_[locate];
        if (!slot) {
            slot = std::make_unique<Book>();
            if (reserve_) slot->reserve(reserve_);
        }
        return *slot;
    }

    const Book* find(uint16_t locate) const {
        return locate < books_.size() ? books_[locate].get() : nullptr;
    }

    size_t book_count() const {
        size_t n = 0;
        for (auto& b : books_) n += b != nullptr;
        return n;
    }

    Result apply(const itch::Message& m) {
        return lob::apply(book(locate_of(m)), m);
    }

    // Visit existing books. Visitor: (locate, Book&).
    template <class F>
    void for_each_book(F&& f) {
        for (size_t i = 0; i < books_.size(); ++i)
            if (books_[i]) f(uint16_t(i), *books_[i]);
    }

private:
    std::vector<std::unique_ptr<Book>> books_;
    size_t reserve_;
};

}  // namespace lob
