#include "book.hpp"

#include <sstream>

namespace lob {

const char* to_string(Result r) {
    switch (r) {
        case Result::Ok:            return "Ok";
        case Result::DuplicateId:   return "DuplicateId";
        case Result::UnknownId:     return "UnknownId";
        case Result::ZeroShares:    return "ZeroShares";
        case Result::TooManyShares: return "TooManyShares";
        case Result::WouldCross:    return "WouldCross";
        case Result::BadReplace:    return "BadReplace";
    }
    return "?";
}

Level& Book::get_level(Side s, uint32_t price) {
    if (s == Side::Buy) {
        auto [it, fresh] = bids_.try_emplace(price);
        if (fresh) it->second.price = price;
        return it->second;
    }
    auto [it, fresh] = asks_.try_emplace(price);
    if (fresh) it->second.price = price;
    return it->second;
}

void Book::push_back(Level& lvl, Order& o) {
    o.level = &lvl;
    o.prev  = lvl.tail;
    o.next  = nullptr;
    if (lvl.tail) lvl.tail->next = &o; else lvl.head = &o;
    lvl.tail = &o;
    lvl.total_shares += o.shares;
    lvl.order_count  += 1;
}

void Book::unlink(Order& o) {
    Level& lvl = *o.level;
    if (o.prev) o.prev->next = o.next; else lvl.head = o.next;
    if (o.next) o.next->prev = o.prev; else lvl.tail = o.prev;
    lvl.total_shares -= o.shares;
    lvl.order_count  -= 1;
    if (lvl.order_count == 0) {
        if (o.side == Side::Buy) bids_.erase(lvl.price);
        else                     asks_.erase(lvl.price);
    }
    o.prev = o.next = nullptr;
    o.level = nullptr;
}

void Book::erase_order(Order& o) {
    uint64_t ref = o.ref;
    unlink(o);
    orders_.erase(ref);
}

Result Book::add(uint64_t ref, Side side, uint32_t shares, uint32_t price) {
    if (shares == 0) return Result::ZeroShares;
    if (orders_.count(ref)) return Result::DuplicateId;
    // A resting add must not cross: crossing flow executes, it never rests.
    if (side == Side::Buy  && !asks_.empty() && price >= best_ask())
        return Result::WouldCross;
    if (side == Side::Sell && !bids_.empty() && price <= best_bid())
        return Result::WouldCross;

    auto [it, ok] = orders_.try_emplace(ref);
    Order& o = it->second;
    o.ref = ref; o.shares = shares; o.price = price; o.side = side;
    o.seq = ++seq_;
    push_back(get_level(side, price), o);
    shares_added_   += shares;
    shares_resting_ += shares;
    return Result::Ok;
}

Result Book::execute(uint64_t ref, uint32_t shares) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return Result::UnknownId;
    if (shares == 0) return Result::ZeroShares;
    Order& o = it->second;
    if (shares > o.shares) return Result::TooManyShares;
    o.shares -= shares;
    o.level->total_shares -= shares;
    shares_executed_ += shares;
    shares_resting_  -= shares;
    if (o.shares == 0) erase_order(o);
    return Result::Ok;
}

Result Book::cancel(uint64_t ref, uint32_t shares) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return Result::UnknownId;
    if (shares == 0) return Result::ZeroShares;
    Order& o = it->second;
    if (shares > o.shares) return Result::TooManyShares;
    o.shares -= shares;
    o.level->total_shares -= shares;
    shares_canceled_ += shares;
    shares_resting_  -= shares;
    if (o.shares == 0) erase_order(o);
    return Result::Ok;
}

Result Book::remove(uint64_t ref) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return Result::UnknownId;
    Order& o = it->second;
    shares_canceled_ += o.shares;
    shares_resting_  -= o.shares;
    erase_order(o);
    return Result::Ok;
}

Result Book::replace(uint64_t orig_ref, uint64_t new_ref,
                     uint32_t shares, uint32_t price) {
    if (orig_ref == new_ref) return Result::BadReplace;
    auto it = orders_.find(orig_ref);
    if (it == orders_.end()) return Result::UnknownId;
    if (orders_.count(new_ref)) return Result::DuplicateId;
    if (shares == 0) return Result::ZeroShares;
    Order& old = it->second;
    Side side = old.side;

    // Cross check must ignore the order being replaced (it leaves the book
    // atomically). Only relevant if old is alone at the best opposite... no:
    // old is on the *same* side, so removing it can only relax the check on
    // its own side. The opposite-side best is unaffected -> plain check works.
    if (side == Side::Buy  && !asks_.empty() && price >= best_ask())
        return Result::WouldCross;
    if (side == Side::Sell && !bids_.empty() && price <= best_bid())
        return Result::WouldCross;

    // ITCH 'U': cancel remainder of orig, add new order at back of queue.
    shares_canceled_ += old.shares;
    shares_resting_  -= old.shares;
    erase_order(old);

    auto [nit, ok] = orders_.try_emplace(new_ref);
    Order& o = nit->second;
    o.ref = new_ref; o.shares = shares; o.price = price; o.side = side;
    o.seq = ++seq_;
    push_back(get_level(side, price), o);
    shares_added_   += shares;
    shares_resting_ += shares;
    return Result::Ok;
}

const Order* Book::find(uint64_t ref) const {
    auto it = orders_.find(ref);
    return it == orders_.end() ? nullptr : &it->second;
}

void Book::for_each_level(
        const std::function<bool(Side, const Level&)>& f) const {
    for (auto& [p, lvl] : bids_) if (!f(Side::Buy, lvl)) return;
    for (auto& [p, lvl] : asks_) if (!f(Side::Sell, lvl)) return;
}

std::string Book::audit() const {
    std::ostringstream err;
    if (crossed()) {
        err << "crossed book: bid " << best_bid() << " >= ask " << best_ask();
        return err.str();
    }
    uint64_t resting = 0;
    size_t   count   = 0;
    bool bad = false;
    auto walk = [&](Side s, const Level& lvl) {
        uint64_t lvl_shares = 0;
        uint32_t lvl_count  = 0;
        uint64_t last_seq   = 0;
        const Order* prev = nullptr;
        for (const Order* o = lvl.head; o; o = o->next) {
            if (o->prev != prev) { err << "broken prev link at ref " << o->ref; bad = true; return false; }
            if (o->level != &lvl) { err << "order " << o->ref << " points at wrong level"; bad = true; return false; }
            if (o->price != lvl.price) { err << "order " << o->ref << " price != level price"; bad = true; return false; }
            if (o->side != s) { err << "order " << o->ref << " on wrong side"; bad = true; return false; }
            if (o->shares == 0) { err << "zero-share order " << o->ref << " resting"; bad = true; return false; }
            if (o->seq <= last_seq) { err << "FIFO violated at ref " << o->ref; bad = true; return false; }
            last_seq = o->seq;
            lvl_shares += o->shares;
            lvl_count  += 1;
            prev = o;
        }
        if (prev != lvl.tail) { err << "tail mismatch at price " << lvl.price; bad = true; return false; }
        if (lvl_shares != lvl.total_shares) { err << "level share sum mismatch at price " << lvl.price; bad = true; return false; }
        if (lvl_count != lvl.order_count) { err << "level count mismatch at price " << lvl.price; bad = true; return false; }
        resting += lvl_shares;
        count   += lvl_count;
        return true;
    };
    for_each_level(walk);
    if (bad) return err.str();
    if (count != orders_.size()) return "order pool size != linked orders";
    if (resting != shares_resting_) return "resting counter != level sums";
    if (shares_added_ != shares_resting_ + shares_executed_ + shares_canceled_)
        return "share conservation violated";
    return {};
}

}  // namespace lob
