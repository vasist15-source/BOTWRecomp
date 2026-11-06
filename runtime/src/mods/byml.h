// BYML documents, the format of most BotW data tables (EventInfo, gamedata flags, save data formats,
// ActorInfo, map units, ...): version 2 to 4, either byte order (BotW's Wii U files are version 2,
// big-endian). Also the YAML text form oead writes them in, which BCML's merge logs use (logs/*.yml in a
// .bnp): `!u 0x10` is a u32, `!l` an s64, `!ul` a u64, `!f64` a double, a plain number with a '.' a float,
// one without an s32, `true`/`false` a bool, `null`/`~` null, anything else (or anything quoted) a string.
// Readers are bounds-checked. The writer lays a document out as oead and roead do (key table, string
// table, then the nodes depth first, equal containers and 64-bit values stored once).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mods::byml {

using Bytes = std::vector<uint8_t>;

enum class Type : uint8_t { Null, String, Binary, Bool, Int, Float, UInt, Int64, UInt64, Double, Array, Hash };

struct Value {
    Type type = Type::Null;
    uint64_t bits = 0;                                // Bool, Int, UInt, Int64, UInt64; Float and Double as their bits
    std::string text;                                 // String, Binary
    std::vector<Value> array;                         // Array
    std::vector<std::pair<std::string, Value>> hash;  // Hash: sorted by key (byte order), each key once

    static Value null() { return {}; }
    static Value string(std::string s);
    static Value boolean(bool b);
    static Value s32(int32_t v);
    static Value u32(uint32_t v);
    static Value f32(float v);
    static Value s64(int64_t v);
    static Value u64(uint64_t v);
    static Value f64(double v);
    static Value new_array();
    static Value new_hash();

    bool is(Type t) const { return type == t; }
    int32_t as_s32() const { return int32_t(uint32_t(bits)); }
    uint32_t as_u32() const { return uint32_t(bits); }
    float as_f32() const;
    double as_f64() const;
    // a number of any integer type (s32, u32, s64, u64) as an int64; false for anything else
    bool integer(int64_t& out) const;

    // Hash: the value at `key`, or null
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    Value& set(const std::string& key, Value v);  // inserts in key order or replaces
    bool erase(std::string_view key);
    // the string at `key` of a Hash, or "" (not a string, or missing)
    const std::string& str(std::string_view key) const;

    bool operator==(const Value& o) const;  // the same type and value (floats by their bits)
    bool operator!=(const Value& o) const { return !(*this == o); }
};

struct Document {
    Value root;  // a Hash, an Array or Null
    uint16_t version = 2;
    bool big_endian = true;
};

bool is_byml(const uint8_t* d, size_t n);  // "BY" / "YB" and a supported version
bool parse(const uint8_t* d, size_t n, Document& out, std::string* why = nullptr);
bool write(const Document& doc, Bytes& out, std::string* why = nullptr);
// oead's YAML text form (see above); a document that is a Hash, an Array or null
bool parse_text(std::string_view text, Value& out, std::string* why = nullptr);

}  // namespace mods::byml
