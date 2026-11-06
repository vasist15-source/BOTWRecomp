// BYML documents and their YAML text form: see byml.h.
//   header (0x10): "BY" (big-endian) / "YB", version, offsets of the hash key table, the string table and the
//   root node (0: none)
//   string table (0xC2): count (24 bits), count + 1 offsets from the table (the last one its end), the strings
//   array (0xC0): count, a type byte per item (padded to 4), a 32-bit value per item
//   hash (0xC1): count, per entry a key index (24 bits), a type byte and a 32-bit value, in key order
//   values: 0xA0 string (index), 0xA1 binary (v4: offset of a size and the bytes), 0xD0 bool, 0xD1 s32, 0xD2 f32,
//   0xD3 u32 inline; 0xD4 s64, 0xD5 u64, 0xD6 f64 (v3+: offset of 8 bytes); 0xC0/0xC1 offset; 0xFF null
#include "byml.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace mods::byml {
namespace {

bool fail(std::string* why, std::string text) {
    if (why) *why = std::move(text);
    return false;
}
size_t align4(size_t v) { return (v + 3) & ~size_t(3); }

uint64_t bits_of(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
uint64_t bits_of(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

constexpr uint8_t kString = 0xA0, kBinary = 0xA1, kArray = 0xC0, kHash = 0xC1, kTable = 0xC2, kBool = 0xD0, kInt = 0xD1,
                  kFloat = 0xD2, kUInt = 0xD3, kInt64 = 0xD4, kUInt64 = 0xD5, kDouble = 0xD6, kNull = 0xFF;

uint8_t code_of(Type t) {
    switch (t) {
    case Type::Null: return kNull;
    case Type::String: return kString;
    case Type::Binary: return kBinary;
    case Type::Bool: return kBool;
    case Type::Int: return kInt;
    case Type::Float: return kFloat;
    case Type::UInt: return kUInt;
    case Type::Int64: return kInt64;
    case Type::UInt64: return kUInt64;
    case Type::Double: return kDouble;
    case Type::Array: return kArray;
    case Type::Hash: return kHash;
    }
    return kNull;
}
bool inline_type(Type t) {
    return t != Type::Array && t != Type::Hash && t != Type::Binary && t != Type::Int64 && t != Type::UInt64 && t != Type::Double;
}

// ---------------------------------------------------------------- binary reader
struct Reader {
    const uint8_t* d;
    size_t n;
    bool be;
    uint16_t version;
    std::string* why;
    std::vector<std::string> keys, strings;
    size_t budget = size_t(1) << 24;  // values in all: shared nodes can't make a small file huge

    uint32_t r32(size_t at) const {
        const uint8_t* p = d + at;
        return be ? uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]
                  : uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
    uint64_t r64(size_t at) const {
        const uint64_t a = r32(at), b = r32(at + 4);
        return be ? a << 32 | b : b << 32 | a;
    }
    uint32_t r24(size_t at) const {  // the 24 bits after a node's type byte, or before a hash entry's type byte
        const uint8_t* p = d + at;
        return be ? uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2] : uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
    }
    bool table(size_t off, std::vector<std::string>& out) {
        if (off == 0) return true;
        if (off + 4 > n || d[off] != kTable) return fail(why, "BYML string table out of range");
        const size_t count = r24(off + 1);
        if (off + 4 + (count + 1) * 4 > n) return fail(why, "BYML string table out of range");
        out.resize(count);
        for (size_t k = 0; k < count; k++) {
            const size_t at = off + r32(off + 4 + 4 * k);
            if (at >= n) return fail(why, "BYML string out of range");
            const void* nul = memchr(d + at, 0, n - at);
            if (!nul) return fail(why, "unterminated BYML string");
            out[k].assign(reinterpret_cast<const char*>(d + at), static_cast<const uint8_t*>(nul) - (d + at));
        }
        return true;
    }
    bool value(uint8_t type, uint32_t v, Value& out, int depth) {
        if (!budget--) return fail(why, "BYML document too large");
        switch (type) {
        case kString:
            if (v >= strings.size()) return fail(why, "BYML string index out of range");
            out.type = Type::String;
            out.text = strings[v];
            return true;
        case kBinary: {
            if (size_t(v) + 4 > n) return fail(why, "BYML binary data out of range");
            const size_t size = r32(v);
            if (size_t(v) + 4 + size > n) return fail(why, "BYML binary data out of range");
            out.type = Type::Binary;
            out.text.assign(reinterpret_cast<const char*>(d + v + 4), size);
            return true;
        }
        case kBool: out.type = Type::Bool; out.bits = v != 0; return true;
        case kInt: out.type = Type::Int; out.bits = v; return true;
        case kFloat: out.type = Type::Float; out.bits = v; return true;
        case kUInt: out.type = Type::UInt; out.bits = v; return true;
        case kInt64: case kUInt64: case kDouble:
            if (size_t(v) + 8 > n) return fail(why, "BYML 64-bit value out of range");
            out.type = type == kInt64 ? Type::Int64 : type == kUInt64 ? Type::UInt64 : Type::Double;
            out.bits = r64(v);
            return true;
        case kNull: out.type = Type::Null; return true;
        case kArray: case kHash: return container(v, out, depth + 1);
        default: return fail(why, "unknown BYML node type " + std::to_string(type));
        }
    }
    bool container(size_t off, Value& out, int depth) {
        if (depth > 64) return fail(why, "BYML nodes nested too deep");
        if (off + 4 > n) return fail(why, "BYML node out of range");
        const uint8_t type = d[off];
        const size_t count = r24(off + 1);
        if (type == kArray) {
            const size_t values = off + 4 + align4(count);
            if (values + count * 4 > n) return fail(why, "BYML array out of range");
            out.type = Type::Array;
            out.array.resize(count);
            for (size_t k = 0; k < count; k++)
                if (!value(d[off + 4 + k], r32(values + 4 * k), out.array[k], depth)) return false;
            return true;
        }
        if (type == kHash) {
            if (off + 4 + count * 8 > n) return fail(why, "BYML hash out of range");
            out.type = Type::Hash;
            out.hash.resize(count);
            for (size_t k = 0; k < count; k++) {
                const size_t at = off + 4 + 8 * k;
                const uint32_t key = r24(at);
                if (key >= keys.size()) return fail(why, "BYML hash key out of range");
                out.hash[k].first = keys[key];
                if (!value(d[at + 3], r32(at + 4), out.hash[k].second, depth)) return false;
            }
            // the game's files list keys in order; keep that order, as Value::find expects
            if (!std::is_sorted(out.hash.begin(), out.hash.end(), [](const auto& a, const auto& b) { return a.first < b.first; }))
                std::stable_sort(out.hash.begin(), out.hash.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            return true;
        }
        return fail(why, "BYML node is not an array or a hash");
    }
};

// ---------------------------------------------------------------- binary writer
struct Writer {
    bool be;
    Bytes out;
    std::vector<std::string_view> keys, strings;
    std::unordered_map<std::string_view, uint32_t> key_at, string_at;
    std::unordered_map<const Value*, uint64_t> hashes;
    std::unordered_map<uint64_t, std::vector<std::pair<const Value*, uint32_t>>> written;  // equal nodes stored once

    void p8(uint8_t v) { out.push_back(v); }
    void p24(uint32_t v) {
        if (be) {
            p8(uint8_t(v >> 16)); p8(uint8_t(v >> 8)); p8(uint8_t(v));
        } else {
            p8(uint8_t(v)); p8(uint8_t(v >> 8)); p8(uint8_t(v >> 16));
        }
    }
    void p32(uint32_t v) {
        const size_t at = out.size();
        out.resize(at + 4);
        patch32(at, v);
    }
    void p64(uint64_t v) {
        if (be) {
            p32(uint32_t(v >> 32)); p32(uint32_t(v));
        } else {
            p32(uint32_t(v)); p32(uint32_t(v >> 32));
        }
    }
    void patch32(size_t at, uint32_t v) {
        uint8_t* p = out.data() + at;
        if (be) {
            p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
        } else {
            p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
        }
    }
    void align() { out.resize(align4(out.size()), 0); }

    void collect(const Value& v, std::vector<std::string_view>& k, std::vector<std::string_view>& s) {
        if (v.type == Type::String) s.push_back(v.text);
        for (const auto& item : v.array) collect(item, k, s);
        for (const auto& [key, item] : v.hash) {
            k.push_back(key);
            collect(item, k, s);
        }
    }
    static void finish(std::vector<std::string_view>& t, std::unordered_map<std::string_view, uint32_t>& at) {
        std::sort(t.begin(), t.end());
        t.erase(std::unique(t.begin(), t.end()), t.end());
        at.reserve(t.size());
        for (size_t k = 0; k < t.size(); k++) at.emplace(t[k], uint32_t(k));
    }
    void table(const std::vector<std::string_view>& t) {
        const size_t start = out.size();
        p8(kTable);
        p24(uint32_t(t.size()));
        const size_t offsets = out.size();
        out.resize(offsets + 4 * (t.size() + 1), 0);
        for (size_t k = 0; k < t.size(); k++) {
            patch32(offsets + 4 * k, uint32_t(out.size() - start));
            out.insert(out.end(), t[k].begin(), t[k].end());
            p8(0);
        }
        patch32(offsets + 4 * t.size(), uint32_t(out.size() - start));
        align();
    }
    uint64_t hash_of(const Value& v) {
        if (auto it = hashes.find(&v); it != hashes.end()) return it->second;
        uint64_t h = 1469598103934665603ull ^ uint64_t(v.type);
        auto mix = [&](uint64_t x) { h = (h ^ x) * 1099511628211ull; };
        mix(v.bits);
        for (unsigned char c : v.text) mix(c);
        for (const auto& item : v.array) mix(hash_of(item));
        for (const auto& [key, item] : v.hash) {
            for (unsigned char c : key) mix(c);
            mix(hash_of(item));
        }
        hashes.emplace(&v, h);
        return h;
    }
    void inline_value(const Value& v) {
        switch (v.type) {
        case Type::String: p32(string_at.at(v.text)); break;
        case Type::Null: p32(0); break;
        default: p32(uint32_t(v.bits)); break;
        }
    }
    void node(const Value& v) {
        std::vector<std::pair<const Value*, size_t>> later;  // non-inline items and where their offsets go
        auto item = [&](const Value& x) {
            if (inline_type(x.type)) {
                inline_value(x);
            } else {
                later.push_back({&x, out.size()});
                p32(0);
            }
        };
        if (v.type == Type::Array) {
            p8(kArray);
            p24(uint32_t(v.array.size()));
            for (const auto& x : v.array) p8(code_of(x.type));
            align();
            for (const auto& x : v.array) item(x);
        } else {
            p8(kHash);
            p24(uint32_t(v.hash.size()));
            for (const auto& [key, x] : v.hash) {
                p24(key_at.at(key));
                p8(code_of(x.type));
                item(x);
            }
        }
        for (const auto& [x, slot] : later) {
            align();
            auto& same = written[hash_of(*x)];
            const auto it = std::find_if(same.begin(), same.end(), [&](const auto& w) { return *w.first == *x; });
            if (it != same.end()) {
                patch32(slot, it->second);
                continue;
            }
            const uint32_t at = uint32_t(out.size());
            patch32(slot, at);
            same.push_back({x, at});
            if (x->type == Type::Array || x->type == Type::Hash) node(*x);
            else if (x->type == Type::Binary) {
                p32(uint32_t(x->text.size()));
                out.insert(out.end(), x->text.begin(), x->text.end());
            } else {
                p64(x->bits);
            }
        }
    }
};

// ---------------------------------------------------------------- YAML text (oead's form)
bool parse_int(std::string_view s, __int128& out) {
    bool neg = false;
    size_t i = 0;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        neg = s[0] == '-';
        i = 1;
    }
    if (s.size() > i + 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        if (neg || i) return false;  // oead takes hex only as 0x...
        __int128 v = 0;
        for (size_t k = i + 2; k < s.size(); k++) {
            const char c = s[k];
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0 || v > (__int128(1) << 100)) return false;
            v = v * 16 + digit;
        }
        out = v;
        return true;
    }
    if (i >= s.size()) return false;
    __int128 v = 0;
    for (size_t k = i; k < s.size(); k++) {
        if (s[k] < '0' || s[k] > '9' || v > (__int128(1) << 100)) return false;
        v = v * 10 + (s[k] - '0');
    }
    out = neg ? -v : v;
    return true;
}
bool parse_float(std::string_view s, double& out) {
    if (s == ".inf" || s == ".Inf" || s == ".INF" || s == "+.inf" || s == "+.Inf" || s == "+.INF") {
        out = INFINITY;
        return true;
    }
    if (s == "-.inf" || s == "-.Inf" || s == "-.INF") {
        out = -INFINITY;
        return true;
    }
    if (s == ".nan" || s == ".NaN" || s == ".NAN") {
        out = NAN;
        return true;
    }
    // [+-] (digits [. digits] | . digits) [e [+-] digits]
    size_t i = 0, digits = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++, digits++;
    if (i < s.size() && s[i] == '.') {
        i++;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++, digits++;
    }
    if (!digits) return false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
        const size_t start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        if (i == start) return false;
    }
    if (i != s.size()) return false;
    out = std::strtod(std::string(s).c_str(), nullptr);
    return true;
}

class TextParser {
public:
    TextParser(std::string_view s, std::string* why) : s_(s), why_(why) {
        size_t at = 0;
        while (at <= s_.size()) {
            size_t end = s_.find('\n', at);
            if (end == std::string_view::npos) end = s_.size();
            size_t stop = end;
            if (stop > at && s_[stop - 1] == '\r') stop--;
            size_t indent = 0;
            while (at + indent < stop && s_[at + indent] == ' ') indent++;
            lines_.push_back({at, stop, indent});
            if (end == s_.size()) break;
            at = end + 1;
        }
    }
    bool document(Value& out) {
        skip_blank();
        if (line_ >= lines_.size()) {
            out = Value::null();
            return true;
        }
        if (!block(0, out)) return false;
        skip_blank();
        if (line_ < lines_.size()) return error("unexpected text");
        return true;
    }

private:
    struct Line {
        size_t begin, end, indent;
    };
    std::string_view s_;
    std::string* why_;
    std::vector<Line> lines_;
    size_t line_ = 0;
    int depth_ = 0;

    bool error(const std::string& text) {
        const size_t at = std::min(line_, lines_.empty() ? size_t(0) : lines_.size() - 1);
        return fail(why_, text + " (line " + std::to_string(at + 1) + ")");
    }
    std::string_view content(const Line& l) const { return s_.substr(l.begin + l.indent, l.end - l.begin - l.indent); }
    static bool blank(std::string_view c) {
        size_t i = 0;
        while (i < c.size() && (c[i] == ' ' || c[i] == '\t')) i++;
        return i == c.size() || c[i] == '#';
    }
    void skip_blank() {
        while (line_ < lines_.size()) {
            const std::string_view c = content(lines_[line_]);
            const bool marker = lines_[line_].indent == 0 && (c == "---" || c == "..." || c.rfind("--- ", 0) == 0 || c.rfind('%', 0) == 0);
            if (!blank(c) && !marker) break;
            line_++;
        }
    }
    static bool seq_item(std::string_view c) { return !c.empty() && c[0] == '-' && (c.size() == 1 || c[1] == ' ' || c[1] == '\t'); }
    size_t line_of(size_t offset) const {  // the line holding an offset of s_
        size_t lo = 0, hi = lines_.size();
        while (hi - lo > 1) {
            const size_t mid = (lo + hi) / 2;
            (lines_[mid].begin <= offset ? lo : hi) = mid;
        }
        return lo;
    }
    // after a node that ended at `offset`: only spaces or a comment may follow on its line
    bool end_of_line(size_t offset) {
        line_ = line_of(offset);
        const Line& l = lines_[line_];
        if (offset < l.end && !blank(s_.substr(offset, l.end - offset))) return error("unexpected text after a value");
        line_++;
        return true;
    }
    // a mapping key at the start of `c`: its text and where the value starts (after ": ")
    bool map_key(std::string_view c, std::string& key, size_t& value_at) {
        if (c.empty() || c[0] == '[' || c[0] == '{' || c[0] == '#' || seq_item(c)) return false;
        size_t i = 0;
        if (c[0] == '\'' || c[0] == '"') {
            std::string k;
            size_t p = 0;
            if (!quoted_inline(c, p, k)) return false;
            i = p;
            while (i < c.size() && (c[i] == ' ' || c[i] == '\t')) i++;
            if (i >= c.size() || c[i] != ':' || (i + 1 < c.size() && c[i + 1] != ' ' && c[i + 1] != '\t')) return false;
            key = std::move(k);
            value_at = i + 1;
            return true;
        }
        for (; i < c.size(); i++) {
            if (c[i] == ':' && (i + 1 == c.size() || c[i + 1] == ' ' || c[i + 1] == '\t')) {
                size_t e = i;
                while (e > 0 && (c[e - 1] == ' ' || c[e - 1] == '\t')) e--;
                if (e == 0) return false;
                key.assign(c.substr(0, e));
                value_at = i + 1;
                return true;
            }
            if (c[i] == '#' && i > 0 && (c[i - 1] == ' ' || c[i - 1] == '\t')) return false;
        }
        return false;
    }
    // a quoted scalar within one line (keys)
    static bool quoted_inline(std::string_view c, size_t& p, std::string& out) {
        const char q = c[p++];
        while (p < c.size()) {
            const char ch = c[p++];
            if (q == '\'' && ch == '\'') {
                if (p < c.size() && c[p] == '\'') {
                    out += '\'';
                    p++;
                    continue;
                }
                return true;
            }
            if (q == '"' && ch == '"') return true;
            if (q == '"' && ch == '\\') {
                if (p >= c.size()) return false;
                std::string tmp;
                if (!escape(c, p, out)) return false;
                continue;
            }
            out += ch;
        }
        return false;
    }
    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) {
            out += char(0xC0 | cp >> 6);
            out += char(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            out += char(0xE0 | cp >> 12);
            out += char(0x80 | (cp >> 6 & 63));
            out += char(0x80 | (cp & 63));
        } else {
            out += char(0xF0 | cp >> 18);
            out += char(0x80 | (cp >> 12 & 63));
            out += char(0x80 | (cp >> 6 & 63));
            out += char(0x80 | (cp & 63));
        }
    }
    // a double-quoted escape after the backslash at c[p - 1]
    static bool escape(std::string_view c, size_t& p, std::string& out) {
        const char e = c[p++];
        auto hex = [&](int digits) {
            if (p + size_t(digits) > c.size()) return false;
            uint32_t v = 0;
            for (int k = 0; k < digits; k++) {
                const char h = c[p++];
                const int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                if (d < 0) return false;
                v = v * 16 + uint32_t(d);
            }
            utf8(out, v);
            return true;
        };
        switch (e) {
        case '0': out += '\0'; return true;
        case 'a': out += '\a'; return true;
        case 'b': out += '\b'; return true;
        case 't': case '\t': out += '\t'; return true;
        case 'n': out += '\n'; return true;
        case 'v': out += '\v'; return true;
        case 'f': out += '\f'; return true;
        case 'r': out += '\r'; return true;
        case 'e': out += '\x1b'; return true;
        case ' ': out += ' '; return true;
        case '"': out += '"'; return true;
        case '/': out += '/'; return true;
        case '\\': out += '\\'; return true;
        case 'N': utf8(out, 0x85); return true;
        case '_': utf8(out, 0xA0); return true;
        case 'L': utf8(out, 0x2028); return true;
        case 'P': utf8(out, 0x2029); return true;
        case 'x': return hex(2);
        case 'u': return hex(4);
        case 'U': return hex(8);
        default: return false;
        }
    }

    bool block(size_t min_indent, Value& out) {
        if (++depth_ > 200) return error("nested too deep");
        struct Guard {
            int& d;
            ~Guard() { d--; }
        } guard{depth_};
        skip_blank();
        if (line_ >= lines_.size() || lines_[line_].indent < min_indent) {
            out = Value::null();
            return true;
        }
        const Line& l = lines_[line_];
        const std::string_view c = content(l);
        std::string key;
        size_t value_at;
        if (seq_item(c)) return sequence(l.indent, out);
        if (map_key(c, key, value_at)) return mapping(l.indent, out);
        size_t end;
        if (!inline_node(l.begin + l.indent, l.indent, out, end)) return false;
        return end_of_line(end);
    }
    bool mapping(size_t indent, Value& out) {
        out = Value::new_hash();
        for (;;) {
            skip_blank();
            if (line_ >= lines_.size()) return true;
            const Line& l = lines_[line_];
            if (l.indent < indent) return true;
            if (l.indent > indent) return error("bad indentation");
            const std::string_view c = content(l);
            if (seq_item(c)) return true;
            std::string key;
            size_t value_at;
            if (!map_key(c, key, value_at)) return error("expected a key");
            const size_t at = l.begin + l.indent + value_at;
            Value v;
            if (blank(s_.substr(at, l.end - at))) {
                line_++;
                skip_blank();
                if (line_ < lines_.size() && lines_[line_].indent == indent && seq_item(content(lines_[line_]))) {
                    if (!sequence(indent, v)) return false;  // a list at the key's indentation
                } else if (!block(indent + 1, v)) {
                    return false;
                }
            } else {
                size_t end;
                if (!inline_node(at, indent, v, end) || !end_of_line(end)) return false;
            }
            out.set(key, std::move(v));
        }
    }
    bool sequence(size_t indent, Value& out) {
        out = Value::new_array();
        for (;;) {
            skip_blank();
            if (line_ >= lines_.size()) return true;
            Line& l = lines_[line_];
            if (l.indent < indent) return true;
            if (l.indent > indent) return error("bad indentation");
            const std::string_view c = content(l);
            if (!seq_item(c)) return true;
            size_t skip = 1;
            while (skip < c.size() && (c[skip] == ' ' || c[skip] == '\t')) skip++;
            Value v;
            if (blank(c.substr(skip))) {
                line_++;
                if (!block(indent + 1, v)) return false;
            } else {
                l.indent += skip;  // the item's text as a line of its own, indented where it starts
                if (!block(l.indent, v)) return false;
            }
            out.array.push_back(std::move(v));
        }
    }

    // ---- inline nodes: scalars, quoted scalars, flow collections, block scalars (|, >)
    void space(size_t& p, bool newlines) {
        while (p < s_.size()) {
            const char c = s_[p];
            if (c == ' ' || c == '\t' || c == '\r' || (newlines && c == '\n')) {
                p++;
            } else if (c == '#' && (p == 0 || s_[p - 1] == ' ' || s_[p - 1] == '\t' || s_[p - 1] == '\n')) {
                while (p < s_.size() && s_[p] != '\n') p++;
                if (!newlines) return;
            } else {
                return;
            }
        }
    }
    bool inline_node(size_t p, size_t indent, Value& out, size_t& end) {
        std::string tag;
        space(p, false);
        if (p < s_.size() && s_[p] == '!') {
            const size_t start = p;
            while (p < s_.size() && s_[p] != ' ' && s_[p] != '\t' && s_[p] != '\n' && s_[p] != '\r') p++;
            tag.assign(s_.substr(start, p - start));
            space(p, false);
        }
        if (p < s_.size() && (s_[p] == '|' || s_[p] == '>')) return block_scalar(p, indent, tag, out, end);
        return flow_node(p, tag, false, out, end);
    }
    bool flow_node(size_t p, std::string tag, bool in_flow, Value& out, size_t& end) {
        if (++depth_ > 200) return error("nested too deep");
        struct Guard {
            int& d;
            ~Guard() { d--; }
        } guard{depth_};
        if (in_flow) space(p, true);
        if (tag.empty() && p < s_.size() && s_[p] == '!') {
            const size_t start = p;
            while (p < s_.size() && s_[p] != ' ' && s_[p] != '\t' && s_[p] != '\n' && s_[p] != ',' && s_[p] != ']' && s_[p] != '}') p++;
            tag.assign(s_.substr(start, p - start));
            space(p, in_flow);
        }
        if (p >= s_.size()) return resolve(tag, "", false, out) && (end = p, true);
        const char c = s_[p];
        if (c == '[') {
            out = Value::new_array();
            p++;
            for (;;) {
                space(p, true);
                if (p >= s_.size()) return error("unterminated [");
                if (s_[p] == ']') {
                    end = p + 1;
                    return true;
                }
                Value item;
                size_t e;
                if (!flow_node(p, {}, true, item, e)) return false;
                p = e;
                space(p, true);
                if (p < s_.size() && s_[p] == ':') {  // a single-pair map in a list
                    Value v;
                    if (!flow_node(p + 1, {}, true, v, e)) return false;
                    p = e;
                    Value pair = Value::new_hash();
                    pair.set(item.type == Type::String ? item.text : std::string(), std::move(v));
                    item = std::move(pair);
                    space(p, true);
                }
                out.array.push_back(std::move(item));
                if (p < s_.size() && s_[p] == ',') {
                    p++;
                    continue;
                }
                if (p < s_.size() && s_[p] == ']') continue;
                return error("expected , or ]");
            }
        }
        if (c == '{') {
            out = Value::new_hash();
            p++;
            for (;;) {
                space(p, true);
                if (p >= s_.size()) return error("unterminated {");
                if (s_[p] == '}') {
                    end = p + 1;
                    return true;
                }
                std::string key;
                bool quoted = false;
                if (!scalar_text(p, true, true, key, quoted, p)) return false;
                space(p, true);
                Value v;
                if (p < s_.size() && s_[p] == ':') {
                    size_t e;
                    if (!flow_node(p + 1, {}, true, v, e)) return false;
                    p = e;
                    space(p, true);
                }
                out.set(key, std::move(v));
                if (p < s_.size() && s_[p] == ',') {
                    p++;
                    continue;
                }
                if (p < s_.size() && s_[p] == '}') continue;
                return error("expected , or }");
            }
        }
        std::string text;
        bool quoted = false;
        if (!scalar_text(p, in_flow, false, text, quoted, end)) return false;
        return resolve(tag, text, quoted, out);
    }
    // a plain or quoted scalar at p; `key`: a flow map key (ends at ':')
    bool scalar_text(size_t p, bool in_flow, bool key, std::string& out, bool& quoted, size_t& end) {
        quoted = false;
        if (p < s_.size() && (s_[p] == '\'' || s_[p] == '"')) {
            quoted = true;
            const char q = s_[p++];
            std::string pending;  // a line break folds into a space
            while (p < s_.size()) {
                const char ch = s_[p++];
                if (ch == '\n' || ch == '\r') {
                    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
                    if (ch == '\r' && p < s_.size() && s_[p] == '\n') p++;
                    size_t breaks = 1;
                    for (;;) {
                        while (p < s_.size() && (s_[p] == ' ' || s_[p] == '\t')) p++;
                        if (p < s_.size() && (s_[p] == '\n' || s_[p] == '\r')) {
                            breaks++;
                            if (s_[p] == '\r' && p + 1 < s_.size() && s_[p + 1] == '\n') p++;
                            p++;
                            continue;
                        }
                        break;
                    }
                    out += breaks == 1 ? std::string(" ") : std::string(breaks - 1, '\n');
                    continue;
                }
                if (q == '\'' && ch == '\'') {
                    if (p < s_.size() && s_[p] == '\'') {
                        out += '\'';
                        p++;
                        continue;
                    }
                    end = p;
                    return true;
                }
                if (q == '"' && ch == '"') {
                    end = p;
                    return true;
                }
                if (q == '"' && ch == '\\') {
                    if (p < s_.size() && (s_[p] == '\n' || s_[p] == '\r')) {  // an escaped line break: joined
                        while (p < s_.size() && (s_[p] == '\n' || s_[p] == '\r' || s_[p] == ' ' || s_[p] == '\t')) p++;
                        continue;
                    }
                    if (p >= s_.size() || !escape(s_, p, out)) return error("bad escape in a string");
                    continue;
                }
                out += ch;
            }
            return error("unterminated string");
        }
        const size_t start = p;
        while (p < s_.size()) {
            const char ch = s_[p];
            if (ch == '\n' || ch == '\r') break;
            if (ch == '#' && p > start && (s_[p - 1] == ' ' || s_[p - 1] == '\t')) break;
            if (in_flow && (ch == ',' || ch == ']' || ch == '}' || ch == '[' || ch == '{')) break;
            if (ch == ':' && (p + 1 >= s_.size() || s_[p + 1] == ' ' || s_[p + 1] == '\t' || s_[p + 1] == '\n' || s_[p + 1] == '\r' ||
                              (in_flow && (s_[p + 1] == ',' || s_[p + 1] == ']' || s_[p + 1] == '}')))) {
                if (in_flow || key) break;
            }
            p++;
        }
        size_t e = p;
        while (e > start && (s_[e - 1] == ' ' || s_[e - 1] == '\t')) e--;
        out.assign(s_.substr(start, e - start));
        end = p;
        return true;
    }
    bool block_scalar(size_t p, size_t indent, const std::string& tag, Value& out, size_t& end) {
        const bool literal = s_[p] == '|';
        p++;
        char chomp = 0;
        size_t explicit_indent = 0;
        while (p < s_.size() && s_[p] != '\n' && s_[p] != ' ' && s_[p] != '#') {
            if (s_[p] == '-' || s_[p] == '+') chomp = s_[p];
            else if (s_[p] >= '1' && s_[p] <= '9') explicit_indent = size_t(s_[p] - '0');
            else if (s_[p] != '\r') return error("bad block scalar header");
            p++;
        }
        size_t l = line_of(p) + 1;
        size_t text_indent = explicit_indent ? indent + explicit_indent : 0;
        std::vector<std::string_view> rows;
        for (; l < lines_.size(); l++) {
            const Line& line = lines_[l];
            const std::string_view whole = s_.substr(line.begin, line.end - line.begin);
            const bool empty = whole.find_first_not_of(' ') == std::string_view::npos;
            if (!text_indent && !empty) {
                if (line.indent <= indent) break;
                text_indent = line.indent;
            }
            if (!empty && line.indent < text_indent) break;
            rows.push_back(empty ? std::string_view() : whole.substr(std::min(text_indent, whole.size())));
        }
        size_t used = rows.size();
        while (used && rows[used - 1].empty()) used--;
        std::string text;
        for (size_t k = 0; k < used; k++) {
            if (k) text += literal || rows[k].empty() || rows[k - 1].empty() ? "\n" : " ";
            text += rows[k];
        }
        if (chomp == '+') text.append(rows.size() - used + (used ? 1 : 0), '\n');
        else if (chomp != '-' && used) text += '\n';
        line_ = l;
        end = l < lines_.size() ? lines_[l].begin : s_.size();
        // the block parser resumes at line_; end_of_line() must not re-read the scalar's lines
        if (!resolve(tag.empty() ? std::string("!!str") : tag, text, true, out)) return false;
        end = l > 0 ? lines_[l - 1].end : 0;
        return true;
    }
    bool resolve(const std::string& tag, const std::string& text, bool quoted, Value& out) {
        __int128 i = 0;
        double f = 0;
        if (!tag.empty()) {
            if (tag == "!u") {
                if (!parse_int(text, i)) return error("bad !u value: " + text);
                out = Value::u32(uint32_t(i));
            } else if (tag == "!l") {
                if (!parse_int(text, i)) return error("bad !l value: " + text);
                out = Value::s64(int64_t(i));
            } else if (tag == "!ul") {
                if (!parse_int(text, i)) return error("bad !ul value: " + text);
                out = Value::u64(uint64_t(i));
            } else if (tag == "!f64") {
                if (!parse_float(text, f)) return error("bad !f64 value: " + text);
                out = Value::f64(f);
            } else if (tag == "!!float" || tag == "tag:yaml.org,2002:float") {
                if (!parse_float(text, f)) return error("bad float: " + text);
                out = Value::f32(float(f));
            } else if (tag == "!!int" || tag == "tag:yaml.org,2002:int") {
                if (!parse_int(text, i)) return error("bad integer: " + text);
                out = Value::s32(int32_t(uint32_t(i)));
            } else if (tag == "!!bool" || tag == "tag:yaml.org,2002:bool") {
                out = Value::boolean(text == "true" || text == "True");
            } else if (tag == "!!null" || tag == "tag:yaml.org,2002:null") {
                out = Value::null();
            } else if (tag == "!!str" || tag == "tag:yaml.org,2002:str") {
                out = Value::string(text);
            } else if (tag == "!!binary" || tag == "tag:yaml.org,2002:binary") {
                std::string bytes;
                if (!base64(text, bytes)) return error("bad !!binary value");
                out.type = Type::Binary;
                out.text = std::move(bytes);
            } else {
                return error("unsupported tag " + tag);
            }
            return true;
        }
        if (quoted) {
            out = Value::string(text);
        } else if (text == "true" || text == "false") {
            out = Value::boolean(text[0] == 't');
        } else if (text.find('.') != std::string::npos && parse_float(text, f)) {
            out = Value::f32(float(f));
        } else if (!text.empty() && parse_int(text, i)) {
            out = Value::s32(int32_t(uint32_t(i)));
        } else if (text.empty() || text == "null" || text == "~" || text == "NULL") {
            out = Value::null();
        } else {
            out = Value::string(text);
        }
        return true;
    }
    static bool base64(const std::string& in, std::string& out) {
        uint32_t acc = 0;
        int bits = 0;
        for (char c : in) {
            int v;
            if (c >= 'A' && c <= 'Z') v = c - 'A';
            else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
            else if (c >= '0' && c <= '9') v = c - '0' + 52;
            else if (c == '+') v = 62;
            else if (c == '/') v = 63;
            else if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
            else return false;
            acc = acc << 6 | uint32_t(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out += char(acc >> bits & 0xFF);
            }
        }
        return true;
    }
};

}  // namespace

Value Value::string(std::string s) {
    Value v;
    v.type = Type::String;
    v.text = std::move(s);
    return v;
}
Value Value::boolean(bool b) {
    Value v;
    v.type = Type::Bool;
    v.bits = b;
    return v;
}
Value Value::s32(int32_t x) {
    Value v;
    v.type = Type::Int;
    v.bits = uint32_t(x);
    return v;
}
Value Value::u32(uint32_t x) {
    Value v;
    v.type = Type::UInt;
    v.bits = x;
    return v;
}
Value Value::f32(float x) {
    Value v;
    v.type = Type::Float;
    v.bits = bits_of(x);
    return v;
}
Value Value::s64(int64_t x) {
    Value v;
    v.type = Type::Int64;
    v.bits = uint64_t(x);
    return v;
}
Value Value::u64(uint64_t x) {
    Value v;
    v.type = Type::UInt64;
    v.bits = x;
    return v;
}
Value Value::f64(double x) {
    Value v;
    v.type = Type::Double;
    v.bits = bits_of(x);
    return v;
}
Value Value::new_array() {
    Value v;
    v.type = Type::Array;
    return v;
}
Value Value::new_hash() {
    Value v;
    v.type = Type::Hash;
    return v;
}
float Value::as_f32() const {
    const uint32_t u = uint32_t(bits);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
double Value::as_f64() const {
    double d;
    memcpy(&d, &bits, 8);
    return d;
}
bool Value::integer(int64_t& out) const {
    switch (type) {
    case Type::Int: out = as_s32(); return true;
    case Type::UInt: out = as_u32(); return true;
    case Type::Int64: case Type::UInt64: out = int64_t(bits); return true;
    default: return false;
    }
}
const Value* Value::find(std::string_view key) const {
    auto it = std::lower_bound(hash.begin(), hash.end(), key, [](const auto& e, std::string_view k) { return std::string_view(e.first) < k; });
    return it != hash.end() && it->first == key ? &it->second : nullptr;
}
Value* Value::find(std::string_view key) { return const_cast<Value*>(static_cast<const Value*>(this)->find(key)); }
Value& Value::set(const std::string& key, Value v) {
    auto it = std::lower_bound(hash.begin(), hash.end(), key, [](const auto& e, const std::string& k) { return e.first < k; });
    if (it != hash.end() && it->first == key) {
        it->second = std::move(v);
        return it->second;
    }
    return hash.insert(it, {key, std::move(v)})->second;
}
bool Value::erase(std::string_view key) {
    auto it = std::lower_bound(hash.begin(), hash.end(), key, [](const auto& e, std::string_view k) { return std::string_view(e.first) < k; });
    if (it == hash.end() || it->first != key) return false;
    hash.erase(it);
    return true;
}
const std::string& Value::str(std::string_view key) const {
    static const std::string none;
    const Value* v = find(key);
    return v && v->type == Type::String ? v->text : none;
}
bool Value::operator==(const Value& o) const {
    return type == o.type && bits == o.bits && text == o.text && array == o.array && hash == o.hash;
}

bool is_byml(const uint8_t* d, size_t n) {
    if (n < 0x10) return false;
    const bool be = d[0] == 'B' && d[1] == 'Y', le = d[0] == 'Y' && d[1] == 'B';
    if (!be && !le) return false;
    const uint16_t version = be ? uint16_t(d[2] << 8 | d[3]) : uint16_t(d[2] | d[3] << 8);
    return version >= 2 && version <= 4;
}

bool parse(const uint8_t* d, size_t n, Document& out, std::string* why) {
    if (n < 0x10 || !((d[0] == 'B' && d[1] == 'Y') || (d[0] == 'Y' && d[1] == 'B'))) return fail(why, "not a BYML document");
    Reader r{d, n, d[0] == 'B', 0, why, {}, {}};
    r.version = r.be ? uint16_t(d[2] << 8 | d[3]) : uint16_t(d[2] | d[3] << 8);
    if (r.version < 2 || r.version > 4) return fail(why, "unsupported BYML version " + std::to_string(r.version));
    if (!r.table(r.r32(4), r.keys) || !r.table(r.r32(8), r.strings)) return false;
    out = {};
    out.version = r.version;
    out.big_endian = r.be;
    const uint32_t root = r.r32(12);
    if (root == 0) return true;
    return r.container(root, out.root, 0);
}

bool write(const Document& doc, Bytes& out, std::string* why) {
    if (doc.root.type != Type::Hash && doc.root.type != Type::Array && doc.root.type != Type::Null)
        return fail(why, "a BYML document must be a hash, an array or null");
    Writer w{doc.big_endian, {}, {}, {}, {}, {}, {}, {}};
    std::vector<std::string_view> keys, strings;
    w.collect(doc.root, keys, strings);
    Writer::finish(keys, w.key_at);
    Writer::finish(strings, w.string_at);
    if (keys.size() > 0xFFFFFF || strings.size() > 0xFFFFFF) return fail(why, "too many strings for BYML");
    w.out.reserve(1 << 16);
    w.p8(doc.big_endian ? 'B' : 'Y');
    w.p8(doc.big_endian ? 'Y' : 'B');
    if (doc.big_endian) {
        w.p8(uint8_t(doc.version >> 8));
        w.p8(uint8_t(doc.version));
    } else {
        w.p8(uint8_t(doc.version));
        w.p8(uint8_t(doc.version >> 8));
    }
    w.p32(0);
    w.p32(0);
    w.p32(0);
    if (doc.root.type == Type::Null) {
        out = std::move(w.out);
        return true;
    }
    if (!keys.empty()) {
        w.patch32(4, uint32_t(w.out.size()));
        w.table(keys);
    }
    if (!strings.empty()) {
        w.patch32(8, uint32_t(w.out.size()));
        w.table(strings);
    }
    w.align();
    w.patch32(12, uint32_t(w.out.size()));
    w.node(doc.root);
    w.align();
    if (w.out.size() > 0xFFFFFFFFull) return fail(why, "BYML document larger than 4 GB");
    out = std::move(w.out);
    return true;
}

bool parse_text(std::string_view text, Value& out, std::string* why) {
    TextParser p(text, why);
    if (!p.document(out)) return false;
    if (out.type != Type::Hash && out.type != Type::Array && out.type != Type::Null) return fail(why, "the document is not a hash or a list");
    return true;
}

}  // namespace mods::byml
