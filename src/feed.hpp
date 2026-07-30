// feed.hpp - apply a decoded ITCH message to a Book.
// Single instrument for now: stock_locate is ignored (Phase 1 runs one book).
#pragma once
#include "book.hpp"
#include "itch.hpp"

namespace lob {

inline Side side_of(char c) { return c == 'B' ? Side::Buy : Side::Sell; }

inline Result apply(Book& b, const itch::Message& m) {
    struct V {
        Book& b;
        Result operator()(const itch::AddOrder& m) const {
            return b.add(m.order_ref, side_of(m.side), m.shares, m.price);
        }
        Result operator()(const itch::AddOrderMpid& m) const {
            return b.add(m.add.order_ref, side_of(m.add.side),
                         m.add.shares, m.add.price);
        }
        Result operator()(const itch::OrderExecuted& m) const {
            return b.execute(m.order_ref, m.shares);
        }
        Result operator()(const itch::OrderExecutedPrice& m) const {
            return b.execute(m.exec.order_ref, m.exec.shares);
        }
        Result operator()(const itch::OrderCancel& m) const {
            return b.cancel(m.order_ref, m.shares);
        }
        Result operator()(const itch::OrderDelete& m) const {
            return b.remove(m.order_ref);
        }
        Result operator()(const itch::OrderReplace& m) const {
            return b.replace(m.orig_order_ref, m.new_order_ref,
                             m.shares, m.price);
        }
    };
    return std::visit(V{b}, m);
}

}  // namespace lob
