// OFTK: the factored-tokenizer contract shared with the tape repo
// (orderflow-lm / github.com/daltonoscar0/tape), reimplemented here so the
// ITCH ingest can emit token files + manifests the tape training half loads
// unchanged. Byte-for-byte compatible: OFTK v2 binary layout, 52-id vocab,
// "orderflow-factored-v2" manifest with tape's exact key order and strings
// (tape's loader is a strict parser over exactly what its writer emits).
// APPLY side only - bin fitting lives in oftk_fit.hpp, included by the fit
// tool and tests alone, so the tokenize path cannot fit bins (tape's
// fit/apply split, kept deliberately).
#ifndef EXCHANGE_OFTK_HPP
#define EXCHANGE_OFTK_HPP

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <istream>
#include <map>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace oftk {

// ---- vocabulary (tape include/orderflow/tokenizer.hpp) ---------------------

enum TokenId : uint16_t {
    UNK = 0,
    BOS = 1,
    EOS = 2,
    SESSION_OPEN = 3,
    SESSION_CLOSE = 4,
    HALT = 5,
    RESUME = 6,
    TYPE_BASE = 7,   // + (msg_type - 1); MsgType 1..6 -> ids 7..12
    SIDE_BASE = 13,  // 13 bid, 14 ask
    PX_BASE = 15,
    PX_INSIDE = 15,  // level index -1 (inside the spread)
    PX_ZERO = 16,    // PX_BASE + 1 + index; indices -1..+10 -> 15..26
    PX_TAIL = 27,    // level index > +10
    SZ_BASE = 28,    // 8 per-ticker quantile bins -> 28..35
    DT_BASE = 36,
    DT_ZERO = 36,  // dt <= 0
    DT_TAIL = 51,  // dt >= last edge; DT buckets 37..50 (14 log-spaced)
    VOCAB_SIZE = 52,
};

// tape MsgType, LOBSTER numbering. The BX ingest emits Add, PartialCancel,
// Delete, ExecVisible only (U expands to Delete+Add; P/Q/H are skipped).
enum class MsgType : uint8_t {
    Add = 1,
    PartialCancel = 2,
    Delete = 3,
    ExecVisible = 4,
    ExecHidden = 5,
    CrossTrade = 6,
    Halt = 7,  // never an event tuple
};

constexpr int kPxMin = -1;
constexpr int kPxMax = 10;
constexpr int kSizeBins = 8;  // -> 7 edges
constexpr int kDtBins = 14;   // -> 14 edges (log-spaced), plus ZERO + TAIL
constexpr int kEventTokens = 5;
constexpr uint32_t kBinVersion = 2;

struct TickerBins {
    int64_t tick_size = 100;  // metadata only since tape v2
    std::vector<int64_t> size_edges;   // exactly 7, strictly increasing, >= 2
    std::vector<int64_t> dt_edges_ns;  // exactly 14, same invariants
    bool valid() const;
};

struct FitInfo {
    std::string messages_file;
    double train_frac = 1.0;
    uint64_t rows_total = 0;
    uint64_t train_end_index = 0;
    int64_t last_train_time_ns = 0;
    int64_t first_eval_time_ns = 0;
};

struct TickerEntry {
    TickerBins bins;
    FitInfo fit;
};

struct Manifest {
    std::map<std::string, TickerEntry> tickers;
};

// Decoded representative event (tape's ApproxEvent).
struct ApproxEvent {
    MsgType type = MsgType::Add;
    int8_t direction = 1;  // +1 = bid side, -1 = ask side
    bool has_ref = false;  // false -> UNK in the PRICE_OFF slot
    int64_t lvl_off = 0;   // signed occupied-level index
    int64_t size = 0;
    int64_t dt_ns = 0;
};

// ---- names -----------------------------------------------------------------

inline const char* token_name(uint16_t id) {
    static char buf[16];
    switch (id) {
        case UNK: return "UNK";
        case BOS: return "BOS";
        case EOS: return "EOS";
        case SESSION_OPEN: return "SESSION_OPEN";
        case SESSION_CLOSE: return "SESSION_CLOSE";
        case HALT: return "HALT";
        case RESUME: return "RESUME";
    }
    if (id >= TYPE_BASE && id < SIDE_BASE) {
        static const char* kTypes[6] = {"TYPE_ADD",         "TYPE_CANCEL",
                                        "TYPE_DELETE",      "TYPE_EXEC",
                                        "TYPE_EXEC_HIDDEN", "TYPE_CROSS"};
        return kTypes[id - TYPE_BASE];
    }
    if (id == SIDE_BASE) return "SIDE_BID";
    if (id == SIDE_BASE + 1) return "SIDE_ASK";
    if (id == PX_TAIL) return "PX_TAIL";
    if (id >= PX_INSIDE && id < PX_TAIL) {
        std::snprintf(buf, sizeof buf, "PX%+d", int(id) - int(PX_ZERO));
        return buf;
    }
    if (id >= SZ_BASE && id < DT_BASE) {
        std::snprintf(buf, sizeof buf, "SZ%d", int(id) - int(SZ_BASE));
        return buf;
    }
    if (id == DT_ZERO) return "DT_ZERO";
    if (id == DT_TAIL) return "DT_TAIL";
    if (id > DT_ZERO && id < DT_TAIL) {
        std::snprintf(buf, sizeof buf, "DT%d", int(id) - int(DT_ZERO) - 1);
        return buf;
    }
    return "?";
}

// ---- bins ------------------------------------------------------------------

namespace detail {

inline bool strictly_increasing_min2(const std::vector<int64_t>& v) {
    if (v.empty() || v[0] < 2) return false;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] <= v[i - 1]) return false;
    return true;
}

}  // namespace detail

// index = number of edges <= x, i.e. bucket in [0, edges.size()].
inline size_t bucket_of(int64_t x, const std::vector<int64_t>& edges) {
    return static_cast<size_t>(
        std::upper_bound(edges.begin(), edges.end(), x) - edges.begin());
}

inline bool TickerBins::valid() const {
    return tick_size > 0 && size_edges.size() == kSizeBins - 1 &&
           dt_edges_ns.size() == kDtBins &&
           detail::strictly_increasing_min2(size_edges) &&
           detail::strictly_increasing_min2(dt_edges_ns);
}

// ---- quantizers ------------------------------------------------------------

inline uint16_t type_token(MsgType t) {
    return static_cast<uint16_t>(TYPE_BASE + static_cast<uint8_t>(t) - 1);
}

inline uint16_t side_token(int8_t direction) {
    return static_cast<uint16_t>(SIDE_BASE + (direction > 0 ? 0 : 1));
}

inline uint16_t px_token(bool has_ref, int64_t lvl_off) {
    if (!has_ref) return UNK;
    if (lvl_off < kPxMin) return PX_INSIDE;
    if (lvl_off > kPxMax) return PX_TAIL;
    return static_cast<uint16_t>(PX_ZERO + lvl_off);
}

inline uint16_t sz_token(int64_t size, const TickerBins& b) {
    return static_cast<uint16_t>(SZ_BASE + bucket_of(size, b.size_edges));
}

inline uint16_t dt_token(int64_t dt_ns, const TickerBins& b) {
    if (dt_ns <= 0) return DT_ZERO;
    return static_cast<uint16_t>(DT_ZERO + 1 + bucket_of(dt_ns, b.dt_edges_ns));
}

namespace detail {

// Representative strictly inside the bucket so re-encoding is the identity.
inline int64_t bucket_rep(size_t bucket, const std::vector<int64_t>& edges) {
    if (bucket == 0) return edges[0] - 1;
    return edges[bucket - 1];
}

}  // namespace detail

// ---- encode / decode / roundtrip -------------------------------------------

inline bool encode_event(const ApproxEvent& e, const TickerBins& b,
                         uint16_t out[5]) {
    if (e.type == MsgType::Halt || !b.valid()) return false;
    out[0] = type_token(e.type);
    out[1] = side_token(e.direction);
    out[2] = px_token(e.has_ref, e.lvl_off);
    out[3] = sz_token(e.size, b);
    out[4] = dt_token(e.dt_ns, b);
    return true;
}

inline bool decode_event(const uint16_t in[5], const TickerBins& b,
                         ApproxEvent& out) {
    if (!b.valid()) return false;
    if (in[0] < TYPE_BASE || in[0] >= SIDE_BASE) return false;
    out.type = static_cast<MsgType>(in[0] - TYPE_BASE + 1);
    if (in[1] != SIDE_BASE && in[1] != SIDE_BASE + 1) return false;
    out.direction = (in[1] == SIDE_BASE) ? int8_t{1} : int8_t{-1};

    if (in[2] == UNK) {
        out.has_ref = false;
        out.lvl_off = 0;
    } else if (in[2] >= PX_INSIDE && in[2] <= PX_TAIL) {
        out.has_ref = true;
        if (in[2] == PX_TAIL) out.lvl_off = kPxMax + 1;
        else out.lvl_off = int64_t(in[2]) - PX_ZERO;
    } else {
        return false;
    }

    if (in[3] < SZ_BASE || in[3] >= DT_BASE) return false;
    out.size = detail::bucket_rep(in[3] - SZ_BASE, b.size_edges);

    if (in[4] < DT_ZERO || in[4] > DT_TAIL) return false;
    if (in[4] == DT_ZERO) out.dt_ns = 0;
    else out.dt_ns = detail::bucket_rep(in[4] - DT_ZERO - 1, b.dt_edges_ns);
    return true;
}

inline bool roundtrip_ok(const uint16_t in[5], const TickerBins& b) {
    ApproxEvent e;
    uint16_t again[5];
    if (!decode_event(in, b, e)) return false;
    if (!encode_event(e, b, again)) return false;
    return std::memcmp(in, again, sizeof again) == 0;
}

// ---- binary token stream (OFTK) --------------------------------------------
// 28-byte header + n uint16 payload, native (little-endian) byte order:
// [0,4) magic "OFTK"; [4,8) u32 version=2; [8,20) char[12] ticker,
// NUL-padded, last byte must be 0; [20,28) u64 count n; [28, 28+2n) tokens.

namespace detail {
constexpr char kMagic[4] = {'O', 'F', 'T', 'K'};
constexpr size_t kTickerBytes = 12;
}  // namespace detail

inline bool write_token_bin(std::ostream& out, const std::string& ticker,
                            const std::vector<uint16_t>& tokens) {
    if (ticker.size() >= detail::kTickerBytes) return false;
    char tk[detail::kTickerBytes] = {0};
    std::memcpy(tk, ticker.data(), ticker.size());
    uint32_t version = kBinVersion;
    uint64_t n = tokens.size();
    out.write(detail::kMagic, 4);
    out.write(reinterpret_cast<const char*>(&version), 4);
    out.write(tk, detail::kTickerBytes);
    out.write(reinterpret_cast<const char*>(&n), 8);
    out.write(reinterpret_cast<const char*>(tokens.data()),
              static_cast<std::streamsize>(n * 2));
    return bool(out);
}

inline bool read_token_bin(std::istream& in, std::string& ticker,
                           std::vector<uint16_t>& tokens, std::string* err) {
    char magic[4];
    uint32_t version = 0;
    char tk[detail::kTickerBytes];
    uint64_t n = 0;
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(&version), 4);
    in.read(tk, detail::kTickerBytes);
    in.read(reinterpret_cast<char*>(&n), 8);
    if (!in || std::memcmp(magic, detail::kMagic, 4) != 0) {
        if (err) *err = "bad magic (not an OFTK token file)";
        return false;
    }
    if (version != kBinVersion) {
        if (err) *err = "unsupported version " + std::to_string(version);
        return false;
    }
    if (tk[detail::kTickerBytes - 1] != 0) {
        if (err) *err = "unterminated ticker field";
        return false;
    }
    ticker.assign(tk);
    tokens.resize(n);
    in.read(reinterpret_cast<char*>(tokens.data()),
            static_cast<std::streamsize>(n * 2));
    if (!in || static_cast<uint64_t>(in.gcount()) != n * 2) {
        if (err) *err = "truncated token payload";
        return false;
    }
    return true;
}

// ---- manifest JSON ---------------------------------------------------------
// Writer emits tape's fixed key order and strings verbatim; tape's strict
// loader parses exactly this subset, so byte-level fidelity here is the
// compatibility contract.

namespace detail {

inline void write_json_string(std::ostream& o, const std::string& s) {
    o << '"';
    for (char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n"; break;
            case '\t': o << "\\t"; break;
            case '\r': o << "\\r"; break;
            default: o << c;
        }
    }
    o << '"';
}

inline void write_edges(std::ostream& o, const std::vector<int64_t>& v) {
    o << '[';
    for (size_t i = 0; i < v.size(); ++i) o << (i ? "," : "") << v[i];
    o << ']';
}

inline std::string format_double(double d) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", d);
    return buf;
}

// Minimal JSON value + strict parser over the emitted subset (tape's).
struct JsonValue {
    enum Kind { Null, Bool, Int, Dbl, Str, Arr, Obj } kind = Null;
    bool b = false;
    int64_t i = 0;
    double d = 0.0;
    std::string s;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* find(const std::string& key) const {
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    double num() const { return kind == Int ? double(i) : d; }
};

class JsonParser {
public:
    JsonParser(const std::string& text, std::string* err)
        : p_(text.c_str()), err_(err) {}

    bool parse(JsonValue& out) {
        if (!value(out)) return false;
        ws();
        if (*p_ != '\0') return fail("trailing content");
        return true;
    }

private:
    const char* p_;
    std::string* err_;

    bool fail(const char* msg) {
        if (err_) *err_ = std::string("manifest JSON: ") + msg;
        return false;
    }
    void ws() {
        while (std::isspace(static_cast<unsigned char>(*p_))) ++p_;
    }
    bool expect(char c) {
        ws();
        if (*p_ != c) return fail("unexpected character");
        ++p_;
        return true;
    }

    bool string(std::string& out) {
        if (!expect('"')) return false;
        out.clear();
        while (*p_ != '"') {
            if (*p_ == '\0') return fail("unterminated string");
            if (*p_ == '\\') {
                ++p_;
                switch (*p_) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    default: return fail("unsupported escape");
                }
                ++p_;
            } else {
                out += *p_++;
            }
        }
        ++p_;
        return true;
    }

    bool value(JsonValue& out) {
        ws();
        switch (*p_) {
            case '{': return object(out);
            case '[': return array(out);
            case '"': out.kind = JsonValue::Str; return string(out.s);
            case 't':
            case 'f': return boolean(out);
            case 'n':
                if (std::strncmp(p_, "null", 4) != 0) return fail("bad token");
                p_ += 4;
                out.kind = JsonValue::Null;
                return true;
            default: return number(out);
        }
    }

    bool boolean(JsonValue& out) {
        out.kind = JsonValue::Bool;
        if (std::strncmp(p_, "true", 4) == 0) {
            out.b = true;
            p_ += 4;
            return true;
        }
        if (std::strncmp(p_, "false", 5) == 0) {
            out.b = false;
            p_ += 5;
            return true;
        }
        return fail("bad token");
    }

    bool number(JsonValue& out) {
        const char* start = p_;
        if (*p_ == '-') ++p_;
        while (std::isdigit(static_cast<unsigned char>(*p_))) ++p_;
        bool floating = false;
        if (*p_ == '.' || *p_ == 'e' || *p_ == 'E') {
            floating = true;
            if (*p_ == '.') {
                ++p_;
                while (std::isdigit(static_cast<unsigned char>(*p_))) ++p_;
            }
            if (*p_ == 'e' || *p_ == 'E') {
                ++p_;
                if (*p_ == '+' || *p_ == '-') ++p_;
                while (std::isdigit(static_cast<unsigned char>(*p_))) ++p_;
            }
        }
        if (p_ == start || (p_ == start + 1 && *start == '-'))
            return fail("bad number");
        std::string tokn(start, p_);
        if (floating) {
            out.kind = JsonValue::Dbl;
            out.d = std::strtod(tokn.c_str(), nullptr);
        } else {
            out.kind = JsonValue::Int;
            out.i = std::strtoll(tokn.c_str(), nullptr, 10);
        }
        return true;
    }

    bool array(JsonValue& out) {
        out.kind = JsonValue::Arr;
        if (!expect('[')) return false;
        ws();
        if (*p_ == ']') {
            ++p_;
            return true;
        }
        while (true) {
            out.arr.emplace_back();
            if (!value(out.arr.back())) return false;
            ws();
            if (*p_ == ',') {
                ++p_;
                continue;
            }
            return expect(']');
        }
    }

    bool object(JsonValue& out) {
        out.kind = JsonValue::Obj;
        if (!expect('{')) return false;
        ws();
        if (*p_ == '}') {
            ++p_;
            return true;
        }
        while (true) {
            std::string key;
            ws();
            if (!string(key) || !expect(':')) return false;
            out.obj.emplace_back(std::move(key), JsonValue{});
            if (!value(out.obj.back().second)) return false;
            ws();
            if (*p_ == ',') {
                ++p_;
                continue;
            }
            return expect('}');
        }
    }
};

inline bool get_i64(const JsonValue& obj, const char* key, int64_t& out) {
    const JsonValue* v = obj.find(key);
    if (!v || v->kind != JsonValue::Int) return false;
    out = v->i;
    return true;
}

inline bool get_edges(const JsonValue& obj, const char* key, size_t n,
                      std::vector<int64_t>& out) {
    const JsonValue* v = obj.find(key);
    if (!v || v->kind != JsonValue::Arr || v->arr.size() != n) return false;
    out.clear();
    for (const JsonValue& e : v->arr) {
        if (e.kind != JsonValue::Int) return false;
        out.push_back(e.i);
    }
    return true;
}

}  // namespace detail

inline void write_manifest(std::ostream& out, const Manifest& m) {
    out << "{\n";
    out << "  \"format\": \"orderflow-factored-v2\",\n";
    out << "  \"vocab_size\": " << VOCAB_SIZE << ",\n";
    out << "  \"price_off_window\": {\"min\": " << kPxMin
        << ", \"max\": " << kPxMax << "},\n";
    out << "  \"note\": \"PRICE_OFF is the signed occupied-level index on "
           "the event's side of orderbook row i-1 (0=at best, +k=k occupied "
           "levels strictly better, -1=inside the spread). Order reference "
           "identity is dropped.\",\n";
    out << "  \"vocab\": [\n";
    for (uint16_t id = 0; id < VOCAB_SIZE; ++id)
        out << "    {\"id\": " << id << ", \"name\": \"" << token_name(id)
            << "\"}" << (id + 1 < VOCAB_SIZE ? "," : "") << "\n";
    out << "  ],\n";
    out << "  \"tickers\": {\n";
    size_t k = 0;
    for (const auto& [ticker, entry] : m.tickers) {
        out << "    ";
        detail::write_json_string(out, ticker);
        out << ": {\n";
        out << "      \"tick_size\": " << entry.bins.tick_size << ",\n";
        out << "      \"size_edges\": ";
        detail::write_edges(out, entry.bins.size_edges);
        out << ",\n      \"dt_edges_ns\": ";
        detail::write_edges(out, entry.bins.dt_edges_ns);
        out << ",\n      \"fit\": {\n";
        out << "        \"messages_file\": ";
        detail::write_json_string(out, entry.fit.messages_file);
        out << ",\n        \"train_frac\": "
            << detail::format_double(entry.fit.train_frac)
            << ",\n        \"rows_total\": " << entry.fit.rows_total
            << ",\n        \"train_end_index\": " << entry.fit.train_end_index
            << ",\n        \"last_train_time_ns\": "
            << entry.fit.last_train_time_ns
            << ",\n        \"first_eval_time_ns\": "
            << entry.fit.first_eval_time_ns << "\n      }\n    }"
            << (++k < m.tickers.size() ? "," : "") << "\n";
    }
    out << "  }\n}\n";
}

inline bool load_manifest(std::istream& in, Manifest& m, std::string* err) {
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    detail::JsonValue root;
    if (!detail::JsonParser(text, err).parse(root)) return false;
    if (root.kind != detail::JsonValue::Obj) {
        if (err) *err = "manifest: root is not an object";
        return false;
    }
    const detail::JsonValue* fmt = root.find("format");
    if (!fmt || fmt->kind != detail::JsonValue::Str ||
        fmt->s != "orderflow-factored-v2") {
        if (err) *err = "manifest: missing/unknown format";
        return false;
    }
    const detail::JsonValue* tickers = root.find("tickers");
    if (!tickers || tickers->kind != detail::JsonValue::Obj) {
        if (err) *err = "manifest: missing tickers object";
        return false;
    }
    m.tickers.clear();
    for (const auto& [ticker, tv] : tickers->obj) {
        if (tv.kind != detail::JsonValue::Obj) {
            if (err) *err = "manifest: ticker entry is not an object";
            return false;
        }
        TickerEntry e;
        int64_t rows_total = 0, train_end = 0;
        const detail::JsonValue* fit = tv.find("fit");
        if (!detail::get_i64(tv, "tick_size", e.bins.tick_size) ||
            !detail::get_edges(tv, "size_edges", kSizeBins - 1,
                               e.bins.size_edges) ||
            !detail::get_edges(tv, "dt_edges_ns", kDtBins,
                               e.bins.dt_edges_ns) ||
            !fit || fit->kind != detail::JsonValue::Obj ||
            !detail::get_i64(*fit, "rows_total", rows_total) ||
            !detail::get_i64(*fit, "train_end_index", train_end) ||
            !detail::get_i64(*fit, "last_train_time_ns",
                             e.fit.last_train_time_ns) ||
            !detail::get_i64(*fit, "first_eval_time_ns",
                             e.fit.first_eval_time_ns)) {
            if (err) *err = "manifest: bad entry for ticker " + ticker;
            return false;
        }
        const detail::JsonValue* mf = fit->find("messages_file");
        const detail::JsonValue* frac = fit->find("train_frac");
        if (!mf || mf->kind != detail::JsonValue::Str || !frac ||
            (frac->kind != detail::JsonValue::Dbl &&
             frac->kind != detail::JsonValue::Int)) {
            if (err) *err = "manifest: bad fit provenance for " + ticker;
            return false;
        }
        e.fit.messages_file = mf->s;
        e.fit.train_frac = frac->num();
        e.fit.rows_total = static_cast<uint64_t>(rows_total);
        e.fit.train_end_index = static_cast<uint64_t>(train_end);
        if (!e.bins.valid()) {
            if (err) *err = "manifest: invalid bins for ticker " + ticker;
            return false;
        }
        m.tickers.emplace(ticker, std::move(e));
    }
    return true;
}

}  // namespace oftk

#endif  // EXCHANGE_OFTK_HPP
