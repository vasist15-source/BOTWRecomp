// MSBT message files and msyt's JSON form of their texts: see msbt.h.
//   header (0x20): "MsgStdBn", byte order mark, 0, encoding, version 3, section count, 0, file size, 10 zero bytes
//   section: magic, body size, 8 zero bytes, the body, 0xAB bytes up to a multiple of 16
//   LBL1: bucket count, per bucket (label count, offset from the body), labels (length byte, name, text index)
//         by bucket: a label's bucket is (hash * 0x492 + byte, over its name) % bucket count
//   ATR1: entry count, bytes per entry; with strings: an offset per entry (from the body), then the strings
//   TXT2: text count, an offset per text (from the body), then the texts
// A text is UTF-16 with control sequences: 0x0E, group, then the group's fields (msyt's botw module).
#include "msbt.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mods::msbt {
namespace {

bool fail(std::string* why, std::string text) {
    if (why) *why = std::move(text);
    return false;
}

struct In {
    const uint8_t* d;
    size_t n;
    bool be;
    uint16_t u16(size_t at) const { return be ? uint16_t(d[at] << 8 | d[at + 1]) : uint16_t(d[at] | d[at + 1] << 8); }
    uint32_t u32(size_t at) const {
        return be ? uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3]
                  : uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24;
    }
};
struct Out {
    bool be;
    Bytes b;
    void u8(uint32_t v) { b.push_back(uint8_t(v)); }
    void u16(uint32_t v) {
        if (be) {
            u8(v >> 8);
            u8(v);
        } else {
            u8(v);
            u8(v >> 8);
        }
    }
    void u32(uint32_t v) {
        if (be) {
            u16(v >> 16);
            u16(v);
        } else {
            u16(v);
            u16(v >> 16);
        }
    }
    void put32(size_t at, uint32_t v) {
        uint8_t* p = b.data() + at;
        if (be) {
            p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
        } else {
            p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
        }
    }
    void bytes(const Bytes& x) { b.insert(b.end(), x.begin(), x.end()); }
};

// ---------------------------------------------------------------- UTF-8 <-> UTF-16
bool utf8_points(std::string_view s, std::vector<uint32_t>& out) {
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        uint32_t cp;
        size_t len;
        if (c < 0x80) cp = c, len = 1;
        else if ((c & 0xE0) == 0xC0) cp = c & 0x1F, len = 2;
        else if ((c & 0xF0) == 0xE0) cp = c & 0x0F, len = 3;
        else if ((c & 0xF8) == 0xF0) cp = c & 0x07, len = 4;
        else return false;
        if (i + len > s.size()) return false;
        for (size_t k = 1; k < len; k++) {
            const uint8_t x = uint8_t(s[i + k]);
            if ((x & 0xC0) != 0x80) return false;
            cp = cp << 6 | (x & 0x3F);
        }
        out.push_back(cp);
        i += len;
    }
    return true;
}
void put_string(Out& o, std::string_view utf8) {
    std::vector<uint32_t> points;
    if (!utf8_points(utf8, points)) {  // not UTF-8: byte by byte (as Latin-1)
        points.assign(utf8.begin(), utf8.end());
        for (auto& p : points) p &= 0xFF;
    }
    for (uint32_t cp : points) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            o.u16(0xD800 | (cp >> 10));
            o.u16(0xDC00 | (cp & 0x3FF));
        } else {
            o.u16(cp);
        }
    }
}
size_t units16(std::string_view utf8) {  // UTF-16 units of a string
    std::vector<uint32_t> points;
    if (!utf8_points(utf8, points)) return utf8.size();
    size_t n = 0;
    for (uint32_t cp : points) n += cp >= 0x10000 ? 2 : 1;
    return n;
}
void append_utf8(std::string& out, uint32_t cp) {
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
bool utf16_string(const std::vector<uint16_t>& u, size_t from, size_t to, std::string& out) {
    for (size_t i = from; i < to; i++) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp < 0xDC00) {
            if (i + 1 >= to || u[i + 1] < 0xDC00 || u[i + 1] >= 0xE000) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xDC00 && cp < 0xE000) {
            return false;
        }
        append_utf8(out, cp);
    }
    return true;
}

// ---------------------------------------------------------------- msyt's control kinds
const char* const kColours[] = {"red", "light_green1", "blue", "grey", "light_green4", "orange", "light_grey"};
const char* const kPauses[] = {"short", "long", "longer"};
struct IconName {
    const char* name;
    int code;  // -1: takes its code ({"a": 10})
};
const IconName kIcons[] = {
    {"l_stick_forward", 0}, {"l_stick_back", 1}, {"l_stick_left", 2}, {"l_stick_right", 3}, {"r_stick_vertical", 4},
    {"r_stick_horizontal", 5}, {"d_pad_up", 6}, {"d_pad_down", 7}, {"d_pad_left", 8}, {"d_pad_right", 9}, {"y", 13},
    {"b", 17}, {"l", 20}, {"r", 21}, {"plus", 23}, {"minus", 24}, {"right_arrow", 25}, {"left_arrow", 26}, {"up_arrow", 27},
    {"l_stick_press", 33}, {"r_stick_press", 34}, {"gamepad", 36}, {"a", -1}, {"x", -1}, {"zl", -1}, {"unknown", -1},
};

struct Encoder {
    Out& o;
    std::string* why;
    bool ok = true;

    bool bad(const std::string& text) {
        ok = false;
        return fail(why, "texts: " + text);
    }
    bool number(const json::Value& v, double max, uint32_t& out, const char* what) {
        if (v.type != json::Value::Number || v.number < 0 || v.number > max || std::floor(v.number) != v.number)
            return bad(std::string("bad number for ") + what);
        out = uint32_t(v.number);
        return true;
    }
    bool u16f(const json::Value& c, const char* key) {
        uint32_t v;
        if (!number(c.get(key), 0xFFFF, v, key)) return false;
        o.u16(v);
        return true;
    }
    bool u32f(const json::Value& c, const char* key) {
        uint32_t v;
        if (!number(c.get(key), 4294967295.0, v, key)) return false;
        o.u32(v);
        return true;
    }
    bool u8s(const json::Value& a, size_t want, const char* key, Bytes* out = nullptr) {  // want 0: any length
        if (a.type != json::Value::Array || (want && a.array.size() != want)) return bad(std::string("bad bytes for ") + key);
        for (const auto& x : a.array) {
            uint32_t v;
            if (!number(x, 255, v, key)) return false;
            if (out) out->push_back(uint8_t(v));
            else o.u8(v);
        }
        return true;
    }
    bool sized_bytes(const json::Value& a, const char* key) {  // a u16 length, then the bytes
        Bytes b;
        if (!u8s(a, 0, key, &b)) return false;
        if (b.size() > 0xFFFF) return bad("too many bytes");
        o.u16(uint32_t(b.size()));
        o.bytes(b);
        return true;
    }
    void sized_string(std::string_view s) {  // its UTF-16 byte length, then the string
        o.u16(uint32_t(units16(s) * 2));
        put_string(o, s);
    }
    void start(uint32_t group, uint32_t type) {
        o.u16(0x0E);
        o.u16(group);
        o.u16(type);
    }
    // the variant of an externally tagged enum: {"name": body}
    static bool variant(const json::Value& v, std::string& name, const json::Value*& body) {
        if (v.type == json::Value::String) {
            name = v.text;
            body = nullptr;
            return true;
        }
        if (v.type != json::Value::Object || v.object.size() != 1) return false;
        name = v.object.begin()->first;
        body = &v.object.begin()->second;
        return true;
    }
    // a tuple variant [kind, {fields}]
    bool tuple(const json::Value& v, uint32_t& kind, const json::Value*& fields) {
        if (v.type != json::Value::Array || v.array.size() != 2 || v.array[1].type != json::Value::Object) return bad("bad control fields");
        fields = &v.array[1];
        return number(v.array[0], 0xFFFF, kind, "kind");
    }

    bool raw(const json::Value& c) {
        std::string group;
        const json::Value* body = nullptr;
        for (const char* g : {"zero", "one", "two", "three", "four", "five", "two_hundred_one"})
            if (c.object.count(g)) {
                group = g;
                body = &c.object.at(g);
            }
        if (!body) return bad("raw control without a group");
        const json::Value& b = *body;
        if (group == "three") {  // field_1 is the type
            o.u16(0x0E);
            o.u16(3);
            return u16f(b, "field_1") && sized_bytes(b.get("field_2"), "field_2");
        }
        if (group == "five") {
            o.u16(0x0E);
            o.u16(5);
            return u16f(b, "field_1") && u16f(b, "field_2");
        }
        std::string sub;
        const json::Value* f = nullptr;
        if (!variant(b, sub, f) || !f) return bad("bad raw control");
        if (group == "zero") {
            const char* const subs[] = {"zero", "one", "two", "three", "four"};
            const auto it = std::find_if(std::begin(subs), std::end(subs), [&](const char* s) { return sub == s; });
            if (it == std::end(subs)) return bad("unknown raw control 0." + sub);
            start(0, uint32_t(it - std::begin(subs)));
            if (sub == "zero") return u16f(*f, "field_1") && u16f(*f, "field_2") && u16f(*f, "field_3");
            if (sub == "four") return u16f(*f, "field_1");
            return u16f(*f, "field_1") && u16f(*f, "field_2");
        }
        if (group == "one") {
            const char* const subs[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"};
            const auto it = std::find_if(std::begin(subs), std::end(subs), [&](const char* s) { return sub == s; });
            if (it == std::end(subs)) return bad("unknown raw control 1." + sub);
            const uint32_t type = uint32_t(it - std::begin(subs));
            start(1, type);
            switch (type) {
            case 0: case 1: case 3: return u16f(*f, "field_1") && u32f(*f, "field_2");
            case 2: return u16f(*f, "field_1");
            case 4: return u16f(*f, "field_1") && u16f(*f, "field_2") && u16f(*f, "field_3") && u8s(f->get("field_4"), 2, "field_4");
            case 5:
                return u16f(*f, "field_1") && u16f(*f, "field_2") && u16f(*f, "field_3") && u16f(*f, "field_4") &&
                       u8s(f->get("field_5"), 2, "field_5");
            case 6:
                return u16f(*f, "field_1") && u16f(*f, "field_2") && u16f(*f, "field_3") && u16f(*f, "field_4") && u16f(*f, "field_5") &&
                       u8s(f->get("field_6"), 2, "field_6");
            case 7: return u16f(*f, "field_1") && u8s(f->get("field_2"), 2, "field_2");
            case 8: {
                const json::Value& unknown = f->get("unknown_1");
                const json::Value& words = f->get("field_1");
                if (unknown.type != json::Value::Array || words.type != json::Value::Array) return bad("bad raw control 1.eight");
                o.u16(uint32_t(unknown.array.size() * 4 + words.array.size() * 2 + 4));
                for (const auto& u : unknown.array)
                    if (!u8s(u, 4, "unknown_1")) return false;
                for (const auto& w : words.array) {
                    uint32_t v;
                    if (!number(w, 0xFFFF, v, "field_1")) return false;
                    o.u16(v);
                }
                return u8s(f->get("field_2"), 4, "field_2");
            }
            case 9: {
                const json::Value& strings = f->get("strings");
                if (strings.type != json::Value::Array || strings.array.size() != 4) return bad("bad raw control 1.nine");
                const json::Value& u1 = f->get("unknown_1");
                const json::Value& u2 = f->get("unknown_2");
                size_t payload = (u1.type == json::Value::Array ? 12 : 0) + (u2.type == json::Value::Array ? 12 : 0) + 4 + 2;
                for (const auto& s : strings.array) payload += 4 + units16(s.get("string").string()) * 2;
                o.u16(uint32_t(payload));
                if (u1.type == json::Value::Array && !u8s(u1, 12, "unknown_1")) return false;
                for (const auto& s : strings.array) {
                    if (!u16f(s, "field_1")) return false;
                    sized_string(s.get("string").string());
                }
                if (!u16f(*f, "field_3") || !u16f(*f, "field_4")) return false;
                if (u2.type == json::Value::Array && !u8s(u2, 12, "unknown_2")) return false;
                return u8s(f->get("field_6"), 2, "field_6");
            }
            case 10: return u16f(*f, "field_1") && u16f(*f, "field_2") && u8s(f->get("field_3"), 2, "field_3");
            }
            return false;
        }
        if (group == "two") {
            uint32_t kind;
            const json::Value* fields;
            if (!tuple(*f, kind, fields)) return false;
            o.u16(0x0E);
            o.u16(2);
            o.u16(kind);
            if (sub == "one_field") return u16f(*fields, "field_1");
            if (sub == "variable") {
                if (!u16f(*fields, "field_1")) return false;
                sized_string(fields->get("string").string());
                return u16f(*fields, "field_3");
            }
            return bad("unknown raw control 2." + sub);
        }
        if (group == "four") {
            const char* const subs[] = {"zero", "one", "two", "three"};
            const auto it = std::find_if(std::begin(subs), std::end(subs), [&](const char* s) { return sub == s; });
            if (it == std::end(subs)) return bad("unknown raw control 4." + sub);
            const uint32_t type = uint32_t(it - std::begin(subs));
            start(4, type);
            if (type == 1) return sized_bytes(f->get("field_1"), "field_1");
            if (type == 3) return u16f(*f, "field_1");
            if (!u16f(*f, "field_1")) return false;
            sized_string(f->get("string").string());
            return true;
        }
        // two_hundred_one
        if (sub == "localisation") {
            if (f->type != json::Value::Array || f->array.size() != 2) return bad("bad raw control 201.localisation");
            uint32_t kind;
            if (!localisation_kind(f->array[0], kind)) return false;
            o.u16(0x0E);
            o.u16(201);
            o.u16(kind);
            return localisation(f->array[1].get("strings"));
        }
        uint32_t kind;
        const json::Value* fields;
        if (!tuple(*f, kind, fields)) return false;
        o.u16(0x0E);
        o.u16(201);
        o.u16(kind);
        if (sub == "one_field") return u16f(*fields, "field_1");
        if (sub == "dynamic") {
            Bytes data;
            if (!u16f(*fields, "len") || !u8s(fields->get("field_2"), 0, "field_2", &data)) return false;
            o.bytes(data);
            return true;
        }
        return bad("unknown raw control 201." + sub);
    }
    bool localisation_kind(const json::Value& v, uint32_t& kind) {
        std::string name;
        const json::Value* body;
        if (!variant(v, name, body)) return bad("bad localisation kind");
        if (name == "gender") kind = 5;
        else if (name == "plural") kind = 6;
        else if (name == "unknown" && body) return number(*body, 0xFFFF, kind, "localisation kind");
        else return bad("bad localisation kind " + name);
        return true;
    }
    bool localisation(const json::Value& strings) {
        if (strings.type != json::Value::Array) return bad("bad localisation options");
        size_t len = 0;
        for (const auto& s : strings.array) len += 2 + units16(s.string()) * 2;
        o.u16(uint32_t(len));
        for (const auto& s : strings.array) sized_string(s.string());
        return true;
    }

    bool control(const json::Value& c) {
        if (c.type != json::Value::Object) return bad("a control is not an object");
        const std::string kind = c.get("kind").string();
        if (kind == "raw") return raw(c);
        if (kind == "set_colour" || kind == "reset_colour") {
            uint32_t colour = 0xFFFF;
            if (kind == "set_colour") {
                const std::string name = c.get("colour").string();
                const auto it = std::find_if(std::begin(kColours), std::end(kColours), [&](const char* s) { return name == s; });
                if (it == std::end(kColours)) return bad("unknown colour " + name);
                colour = uint32_t(it - std::begin(kColours));
            }
            start(0, 3);
            o.u16(2);
            o.u16(colour);
            return true;
        }
        if (kind == "pause") {
            if (c.object.count("frames")) {
                start(1, 0);
                o.u16(4);
                return u32f(c, "frames");
            }
            const std::string length = c.get("length").string();
            const auto it = std::find_if(std::begin(kPauses), std::end(kPauses), [&](const char* s) { return length == s; });
            if (it == std::end(kPauses)) return bad("unknown pause " + length);
            o.u16(0x0E);
            o.u16(5);
            o.u16(uint32_t(it - std::begin(kPauses)));
            o.u16(0);
            return true;
        }
        if (kind == "icon") {
            std::string name;
            const json::Value* body;
            if (!variant(c.get("icon"), name, body)) return bad("bad icon");
            const auto it = std::find_if(std::begin(kIcons), std::end(kIcons), [&](const IconName& i) { return name == i.name; });
            if (it == std::end(kIcons)) return bad("unknown icon " + name);
            uint32_t code = uint32_t(it->code);
            if (it->code < 0 && (!body || !number(*body, 255, code, "icon"))) return bad("icon " + name + " without its code");
            start(1, 7);
            o.u16(2);
            o.u8(code);
            o.u8(205);
            return true;
        }
        if (kind == "variable") {
            uint32_t v;
            if (!number(c.get("variable_kind"), 0xFFFF, v, "variable_kind")) return false;
            const std::string name = c.get("name").string();
            start(2, v);
            o.u16(uint32_t(units16(name) * 2 + 4));
            sized_string(name);
            o.u16(0);
            return true;
        }
        if (kind == "choice") {
            const json::Value& labels = c.get("choice_labels");
            if (labels.type != json::Value::Array || labels.array.size() < 2 || labels.array.size() > 4)
                return bad("a choice needs 2 to 4 labels");
            start(1, uint32_t(labels.array.size() + 2));
            if (!u16f(c, "unknown")) return false;
            for (const auto& l : labels.array) {
                uint32_t v;
                if (!number(l, 0xFFFF, v, "choice label")) return false;
                o.u16(v);
            }
            uint32_t selected, cancel;
            if (!number(c.get("selected_index"), 255, selected, "selected_index") || !number(c.get("cancel_index"), 255, cancel, "cancel_index"))
                return false;
            o.u8(selected);
            o.u8(cancel);
            return true;
        }
        if (kind == "single_choice") {
            start(1, 10);
            o.u16(4);
            if (!u16f(c, "label")) return false;
            o.u8(1);
            o.u8(205);
            return true;
        }
        if (kind == "sound") {
            o.u16(0x0E);
            o.u16(3);
            o.u16(1);
            return sized_bytes(c.get("unknown"), "unknown");
        }
        if (kind == "sound2") {
            start(4, 1);
            return sized_bytes(c.get("unknown"), "unknown");
        }
        if (kind == "animation") {
            const std::string name = c.get("name").string();
            start(4, 2);
            o.u16(uint32_t(units16(name) * 2 + 2));
            sized_string(name);
            return true;
        }
        if (kind == "text_size") {
            start(0, 2);
            o.u16(2);
            return u16f(c, "percent");
        }
        if (kind == "auto_advance") {
            start(1, 3);
            o.u16(4);
            return u32f(c, "frames");
        }
        if (kind == "localisation") {
            uint32_t k;
            if (!localisation_kind(c.get("localisation_kind"), k)) return false;
            o.u16(0x0E);
            o.u16(201);
            o.u16(k);
            return localisation(c.get("options"));
        }
        if (kind == "font") {
            const std::string font = c.get("font_kind").string();
            if (font != "normal" && font != "hylian") return bad("unknown font " + font);
            start(0, 1);
            o.u16(2);
            o.u16(font == "normal" ? 0xFFFF : 0);
            return true;
        }
        return bad("unknown control kind " + kind);
    }
};

// ---------------------------------------------------------------- decoding (msyt's parse_controls)
struct Decoder {
    const std::vector<uint16_t>& u;  // the text's units (its 0 unit included)
    const Bytes& raw;
    bool be;
    std::string* why;
    size_t p = 0;  // unit index

    bool bad(const char* text) { return fail(why, std::string("texts: ") + text); }
    bool need(size_t units) { return p + units <= u.size(); }
    uint16_t next() { return u[p++]; }
    uint32_t next32() {
        const uint32_t a = next(), b = next();
        return be ? a << 16 | b : b << 16 | a;
    }
    // bytes at the current unit (2 per unit), `count` of them: must end on a unit boundary for what follows
    bool bytes(size_t count, json::Value& out) {
        const size_t at = p * 2;
        if (at + count > raw.size() || count % 2) return bad("control bytes out of range");
        out.type = json::Value::Array;
        for (size_t k = 0; k < count; k++) out.array.emplace_back(double(raw[at + k]));
        p += count / 2;
        return true;
    }
    bool pair_bytes(json::Value& out) {  // 2 bytes, as read in file order
        return bytes(2, out);
    }
    bool string(size_t bytes_len, std::string& out) {
        if (bytes_len % 2 || !need(bytes_len / 2)) return bad("control string out of range");
        std::vector<uint16_t> units(u.begin() + long(p), u.begin() + long(p + bytes_len / 2));
        if (!utf16_string(units, 0, units.size(), out)) return bad("bad UTF-16 in a control");
        p += bytes_len / 2;
        return true;
    }
    static json::Value num(double v) { return json::Value(v); }
    json::Value raw_group(const char* group, const char* sub, json::Value fields) {
        json::Value c;
        c["kind"] = "raw";
        json::Value inner;
        inner[sub] = std::move(fields);
        c[group] = std::move(inner);
        return c;
    }
    bool control(json::Value& c) {
        if (!need(1)) return bad("short control");
        const uint16_t group = next();
        if (group == 3 || group == 5) {
            if (!need(2)) return bad("short control");
            const uint16_t f1 = next(), f2 = next();
            if (group == 5) {
                if (f2 == 0 && f1 < 3) {
                    c["kind"] = "pause";
                    c["length"] = kPauses[f1];
                    return true;
                }
                c["kind"] = "raw";
                c["five"]["field_1"] = num(f1);
                c["five"]["field_2"] = num(f2);
                return true;
            }
            json::Value data;
            if (!bytes(f2, data)) return false;
            if (f1 == 1) {
                c["kind"] = "sound";
                c["unknown"] = std::move(data);
                return true;
            }
            c["kind"] = "raw";
            c["three"]["field_1"] = num(f1);
            c["three"]["field_2"] = std::move(data);
            return true;
        }
        if (!need(1)) return bad("short control");
        const uint16_t type = next();
        json::Value f;
        if (group == 0) {
            const char* const subs[] = {"zero", "one", "two", "three", "four"};
            if (type > 4) return bad("unknown control 0 type");
            const size_t count = type == 0 ? 3 : type == 4 ? 1 : 2;
            if (!need(count)) return bad("short control");
            const uint16_t f1 = next(), f2 = count > 1 ? next() : 0, f3 = count > 2 ? next() : 0;
            if (type == 1 && f1 == 2 && (f2 == 0xFFFF || f2 == 0)) {
                c["kind"] = "font";
                c["font_kind"] = f2 == 0xFFFF ? "normal" : "hylian";
                return true;
            }
            if (type == 2 && f1 == 2) {
                c["kind"] = "text_size";
                c["percent"] = num(f2);
                return true;
            }
            if (type == 3 && f1 == 2 && (f2 == 0xFFFF || f2 < 7)) {
                c["kind"] = f2 == 0xFFFF ? "reset_colour" : "set_colour";
                if (f2 != 0xFFFF) c["colour"] = kColours[f2];
                return true;
            }
            f["field_1"] = num(f1);
            if (count > 1) f["field_2"] = num(f2);
            if (count > 2) f["field_3"] = num(f3);
            c = raw_group("zero", subs[type], std::move(f));
            return true;
        }
        if (group == 1) {
            const char* const subs[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"};
            if (type > 10) return bad("unknown control 1 type");
            switch (type) {
            case 0: case 1: case 3: {
                if (!need(3)) return bad("short control");
                const uint16_t f1 = next();
                const uint32_t f2 = next32();
                if (f1 == 4 && type != 1) {
                    c["kind"] = type == 0 ? "pause" : "auto_advance";
                    c[type == 0 ? "frames" : "frames"] = num(f2);
                    return true;
                }
                f["field_1"] = num(f1);
                f["field_2"] = num(f2);
                break;
            }
            case 2:
                if (!need(1)) return bad("short control");
                f["field_1"] = num(next());
                break;
            case 4: case 5: case 6: {
                const size_t labels = type - 2;
                if (!need(1 + labels + 1)) return bad("short control");
                const uint16_t f1 = next();
                json::Value l;
                l.type = json::Value::Array;
                for (size_t k = 0; k < labels; k++) l.array.push_back(num(next()));
                json::Value sel;
                if (!pair_bytes(sel)) return false;
                c["kind"] = "choice";
                c["choice_labels"] = std::move(l);
                c["selected_index"] = sel.array[0];
                c["cancel_index"] = sel.array[1];
                c["unknown"] = num(f1);
                return true;
            }
            case 7: {
                if (!need(2)) return bad("short control");
                const uint16_t f1 = next();
                json::Value b;
                if (!pair_bytes(b)) return false;
                if (f1 == 2 && b.array[1].number == 205) {
                    const int code = int(b.array[0].number);
                    json::Value icon;
                    const char* name = nullptr;
                    for (const auto& i : kIcons)
                        if (i.code == code) name = i.name;
                    if (name) icon = name;
                    else {
                        const char* group_name = code == 10 || code == 11 ? "a" : code == 12 || code == 37 || code == 38 ? "x" : code == 14 || code == 15 ? "zl" : "unknown";
                        icon[group_name] = num(code);
                    }
                    c["kind"] = "icon";
                    c["icon"] = std::move(icon);
                    return true;
                }
                f["field_1"] = num(f1);
                f["field_2"] = std::move(b);
                break;
            }
            case 8: {
                if (!need(1)) return bad("short control");
                const uint16_t len = next();
                if (len < 4 || len % 2) return bad("bad control 1.8 length");
                const size_t body = len - 4;
                if (p * 2 + body + 4 > raw.size()) return bad("short control");
                size_t unknown = 0;
                for (size_t k = 0; k + 4 <= body; k += 4) {
                    const uint8_t* q = raw.data() + p * 2 + k;
                    const bool is = q[0] == 255 && q[1] == 255 && q[2] == 0 && q[3] == 0;
                    if (k > 0 && unknown == 0) break;
                    if (!is && unknown > 0) break;
                    if (is) unknown++;
                }
                json::Value u1, words;
                u1.type = json::Value::Array;
                words.type = json::Value::Array;
                for (size_t k = 0; k < unknown; k++) {
                    json::Value chunk;
                    if (!bytes(4, chunk)) return false;
                    u1.array.push_back(std::move(chunk));
                }
                if ((body - unknown * 4) % 2) return bad("bad control 1.8 words");
                for (size_t k = 0; k < (body - unknown * 4) / 2; k++) words.array.push_back(num(next()));
                json::Value f2;
                if (!bytes(4, f2)) return false;
                f["unknown_1"] = std::move(u1);
                f["field_1"] = std::move(words);
                f["field_2"] = std::move(f2);
                break;
            }
            case 9: {
                if (!need(1)) return bad("short control");
                next();  // payload length
                const size_t body_end = raw.size();  // msyt's "rest of the text" bound
                auto unknown12 = [&](json::Value& out) {
                    const size_t at = p * 2;
                    if (at + 12 > raw.size()) return false;
                    static const uint8_t k[12] = {255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0};
                    if (memcmp(raw.data() + at, k, 12)) return false;
                    bytes(12, out);
                    return true;
                };
                json::Value u1;
                if (!unknown12(u1)) u1 = json::Value();
                json::Value strings;
                strings.type = json::Value::Array;
                for (int k = 0; k < 4; k++) {
                    if (!need(2)) return bad("short control");
                    json::Value s;
                    s["field_1"] = num(next());
                    const uint16_t len = next();
                    std::string text;
                    if (!string(len, text)) return false;
                    s["string"] = text;
                    strings.array.push_back(std::move(s));
                }
                if (!need(2)) return bad("short control");
                const uint16_t f3 = next(), f4 = next();
                json::Value u2;
                if (!(body_end > p * 2 + 12 && unknown12(u2))) u2 = json::Value();
                json::Value f6;
                if (!pair_bytes(f6)) return false;
                f["unknown_1"] = std::move(u1);
                f["strings"] = std::move(strings);
                f["field_3"] = num(f3);
                f["field_4"] = num(f4);
                f["unknown_2"] = std::move(u2);
                f["field_6"] = std::move(f6);
                break;
            }
            case 10: {
                if (!need(3)) return bad("short control");
                const uint16_t f1 = next(), f2 = next();
                json::Value f3;
                if (!pair_bytes(f3)) return false;
                if (f1 == 4 && f3.array[0].number == 1 && f3.array[1].number == 205) {
                    c["kind"] = "single_choice";
                    c["label"] = num(f2);
                    return true;
                }
                f["field_1"] = num(f1);
                f["field_2"] = num(f2);
                f["field_3"] = std::move(f3);
                break;
            }
            }
            c = raw_group("one", subs[type], std::move(f));
            return true;
        }
        if (group == 2) {
            if (type == 3 || type == 4 || type == 7 || type == 8 || type == 10 || type == 13) {
                if (!need(1)) return bad("short control");
                json::Value t;
                t.type = json::Value::Array;
                t.array.push_back(num(type));
                json::Value one;
                one["field_1"] = num(next());
                t.array.push_back(std::move(one));
                c["kind"] = "raw";
                c["two"]["one_field"] = std::move(t);
                return true;
            }
            if (type == 1 || type == 2 || type == 9 || type == 11 || type == 12 || (type >= 14 && type <= 19)) {
                if (!need(2)) return bad("short control");
                const uint16_t f1 = next(), len = next();
                std::string name;
                if (!string(len, name) || !need(1)) return false || bad("short control");
                const uint16_t f3 = next();
                if (f3 == 0) {
                    c["kind"] = "variable";
                    c["variable_kind"] = num(type);
                    c["name"] = name;
                    return true;
                }
                json::Value t;
                t.type = json::Value::Array;
                t.array.push_back(num(type));
                json::Value v;
                v["field_1"] = num(f1);
                v["string"] = name;
                v["field_3"] = num(f3);
                t.array.push_back(std::move(v));
                c["kind"] = "raw";
                c["two"]["variable"] = std::move(t);
                return true;
            }
            return bad("unknown control 2 type");
        }
        if (group == 4) {
            if (type > 3) return bad("unknown control 4 type");
            if (type == 1) {
                if (!need(1)) return bad("short control");
                json::Value data;
                if (!bytes(next(), data)) return false;
                c["kind"] = "sound2";
                c["unknown"] = std::move(data);
                return true;
            }
            if (type == 3) {
                if (!need(1)) return bad("short control");
                f["field_1"] = num(next());
                c = raw_group("four", "three", std::move(f));
                return true;
            }
            if (!need(2)) return bad("short control");
            const uint16_t f1 = next(), len = next();
            std::string text;
            if (!string(len, text)) return false;
            if (type == 2) {
                c["kind"] = "animation";
                c["name"] = text;
                return true;
            }
            f["field_1"] = num(f1);
            f["string"] = text;
            c = raw_group("four", "zero", std::move(f));
            return true;
        }
        if (group == 201) {
            if (type >= 5 && type <= 8) {
                if (!need(1)) return bad("short control");
                const uint16_t len = next();
                json::Value options;
                options.type = json::Value::Array;
                size_t total = 0;
                while (total < len) {
                    if (!need(1)) return bad("short control");
                    const uint16_t slen = next();
                    total += size_t(slen) + 2;
                    std::string s;
                    if (slen && !string(slen, s)) return false;
                    options.array.emplace_back(s);
                }
                if (total != len) return bad("bad localisation length");
                c["kind"] = "localisation";
                if (type == 5) c["localisation_kind"] = "gender";
                else if (type == 6) c["localisation_kind"] = "plural";
                else c["localisation_kind"]["unknown"] = num(type);
                c["options"] = std::move(options);
                return true;
            }
            json::Value t;
            t.type = json::Value::Array;
            t.array.push_back(num(type));
            json::Value v;
            if (type == 0) {
                if (!need(1)) return bad("short control");
                const uint16_t len = next();
                json::Value data;
                if (!bytes(len, data)) return false;
                v["len"] = num(len);
                v["field_2"] = std::move(data);
                t.array.push_back(std::move(v));
                c["kind"] = "raw";
                c["two_hundred_one"]["dynamic"] = std::move(t);
                return true;
            }
            if (type <= 4) {
                if (!need(1)) return bad("short control");
                v["field_1"] = num(next());
                t.array.push_back(std::move(v));
                c["kind"] = "raw";
                c["two_hundred_one"]["one_field"] = std::move(t);
                return true;
            }
            return bad("unknown control 201 type");
        }
        return bad("unknown control group");
    }
};

void pad16(Out& o) {
    while (o.b.size() % 16) o.b.push_back(0xAB);
}

}  // namespace

uint32_t label_hash(std::string_view label, uint32_t groups) {
    uint32_t h = 0;
    for (unsigned char c : label) h = h * 0x492 + c;
    return groups ? h % groups : 0;
}

bool parse(const uint8_t* d, size_t n, File& out, std::string* why) {
    if (n < 0x20 || memcmp(d, "MsgStdBn", 8)) return fail(why, "not an MSBT file");
    const bool be = d[8] == 0xFE && d[9] == 0xFF, le = d[8] == 0xFF && d[9] == 0xFE;
    if (!be && !le) return fail(why, "MSBT without a byte order mark");
    In in{d, n, be};
    out = {};
    out.big_endian = be;
    out.unknown1 = in.u16(10);
    out.encoding = d[12];
    out.version = d[13];
    if (out.encoding != 1) return fail(why, "MSBT not in UTF-16");
    const size_t sections = in.u16(14);
    out.unknown3 = in.u16(16);
    memcpy(out.reserved, d + 22, 10);
    out.atr1 = false;
    std::vector<std::pair<std::string, uint32_t>> labels;  // name, text index (LBL1 order)
    std::vector<uint32_t> buckets;
    std::vector<Bytes> texts, attributes;
    bool have_lbl = false, have_txt = false;
    size_t at = 0x20;
    for (size_t s = 0; s < sections; s++) {
        if (at + 16 > n) return fail(why, "MSBT section out of range");
        const std::string magic(reinterpret_cast<const char*>(d + at), 4);
        const size_t size = in.u32(at + 4), body = at + 16;
        if (body + size > n) return fail(why, "MSBT section out of range");
        if (magic == "LBL1") {
            have_lbl = true;
            if (size < 4) return fail(why, "bad LBL1");
            out.groups = in.u32(body);
            if (out.groups == 0 || 4 + size_t(out.groups) * 8 > size) return fail(why, "bad LBL1");
            for (uint32_t g = 0; g < out.groups; g++) {
                const uint32_t count = in.u32(body + 4 + 8 * g);
                size_t p = body + in.u32(body + 8 + 8 * g);
                for (uint32_t k = 0; k < count; k++) {
                    if (p + 1 > body + size) return fail(why, "LBL1 label out of range");
                    const size_t len = d[p];
                    if (p + 1 + len + 4 > body + size) return fail(why, "LBL1 label out of range");
                    labels.push_back({std::string(reinterpret_cast<const char*>(d + p + 1), len), in.u32(p + 1 + len)});
                    buckets.push_back(g);
                    p += 1 + len + 4;
                }
            }
            out.sections.push_back({magic, {}});
        } else if (magic == "ATR1") {
            out.atr1 = true;
            if (size < 8) return fail(why, "bad ATR1");
            const uint32_t count = in.u32(body);
            out.atr_size = in.u32(body + 4);
            if (size > 8 && out.atr_size > 0) {
                if (out.atr_size != 4 || 8 + size_t(count) * 4 > size) return fail(why, "unsupported ATR1");
                out.atr_strings = true;
                for (uint32_t k = 0; k < count; k++) {
                    const size_t from = in.u32(body + 8 + 4 * k), to = k + 1 < count ? in.u32(body + 12 + 4 * k) : size;
                    if (from > to || to > size) return fail(why, "ATR1 string out of range");
                    attributes.emplace_back(d + body + from, d + body + to);
                }
            }
            out.sections.push_back({magic, {}});
        } else if (magic == "TXT2") {
            have_txt = true;
            if (size < 4) return fail(why, "bad TXT2");
            const uint32_t count = in.u32(body);
            if (4 + size_t(count) * 4 > size) return fail(why, "bad TXT2");
            for (uint32_t k = 0; k < count; k++) {
                const size_t from = in.u32(body + 4 + 4 * k), to = k + 1 < count ? in.u32(body + 8 + 4 * k) : size;
                if (from > to || to > size) return fail(why, "TXT2 text out of range");
                texts.emplace_back(d + body + from, d + body + to);
            }
            out.sections.push_back({magic, {}});
        } else {
            out.sections.push_back({magic, Bytes(d + body, d + body + size)});
        }
        at = (body + size + 15) & ~size_t(15);
    }
    if (!have_lbl || !have_txt) return fail(why, "MSBT without labels or texts");
    if (out.atr_strings && attributes.size() != texts.size()) return fail(why, "ATR1 and TXT2 counts differ");
    out.entries.resize(texts.size());
    std::vector<bool> named(texts.size(), false);
    for (size_t k = 0; k < labels.size(); k++) {
        const uint32_t index = labels[k].second;
        if (index >= texts.size() || named[index]) return fail(why, "a label names a missing or shared text");
        named[index] = true;
        out.entries[index].label = labels[k].first;
        out.entries[index].bucket = buckets[k];
        out.label_order.push_back(index);
    }
    if (std::find(named.begin(), named.end(), false) != named.end()) return fail(why, "a text without a label");
    for (size_t k = 0; k < texts.size(); k++) {
        out.entries[k].text = std::move(texts[k]);
        if (out.atr_strings) out.entries[k].attribute = std::move(attributes[k]);
    }
    return true;
}

bool write(const File& f, Bytes& out, std::string* why) {
    Out o{f.big_endian, {}};
    o.b.reserve(4096);
    o.b.insert(o.b.end(), {'M', 's', 'g', 'S', 't', 'd', 'B', 'n'});
    o.u8(f.big_endian ? 0xFE : 0xFF);
    o.u8(f.big_endian ? 0xFF : 0xFE);
    o.u16(f.unknown1);
    o.u8(f.encoding);
    o.u8(f.version);
    o.u16(uint32_t(f.sections.size()));
    o.u16(f.unknown3);
    o.u32(0);  // file size
    o.b.insert(o.b.end(), f.reserved, f.reserved + 10);
    const uint32_t count = uint32_t(f.entries.size());
    for (const auto& [magic, raw] : f.sections) {
        o.b.insert(o.b.end(), magic.begin(), magic.end());
        const size_t size_at = o.b.size();
        o.u32(0);
        o.b.insert(o.b.end(), 8, 0);
        const size_t body = o.b.size();
        if (magic == "LBL1") {
            std::vector<size_t> order = f.label_order;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return f.entries[a].bucket < f.entries[b].bucket; });
            o.u32(f.groups);
            const size_t table = o.b.size();
            o.b.resize(table + 8 * size_t(f.groups), 0);
            std::vector<uint32_t> counts(f.groups, 0), offsets(f.groups, 0);
            size_t k = 0;
            for (uint32_t g = 0; g < f.groups; g++) {
                offsets[g] = uint32_t(o.b.size() - body);
                for (; k < order.size() && f.entries[order[k]].bucket == g; k++) {
                    const Entry& e = f.entries[order[k]];
                    if (e.label.empty() || e.label.size() > 255) return fail(why, "an MSBT label is empty or too long");
                    o.u8(uint32_t(e.label.size()));
                    o.b.insert(o.b.end(), e.label.begin(), e.label.end());
                    o.u32(uint32_t(order[k]));
                    counts[g]++;
                }
            }
            if (k != order.size()) return fail(why, "an MSBT label in a bucket out of range");
            for (uint32_t g = 0; g < f.groups; g++) {
                o.put32(table + 8 * g, counts[g]);
                o.put32(table + 8 * g + 4, offsets[g]);
            }
        } else if (magic == "ATR1") {
            o.u32(count);
            o.u32(f.atr_size);
            if (f.atr_strings) {
                size_t offset = 8 + 4 * size_t(count);
                for (const auto& e : f.entries) {
                    o.u32(uint32_t(offset));
                    offset += e.attribute.size();
                }
                for (const auto& e : f.entries) o.bytes(e.attribute);
            }
        } else if (magic == "TXT2") {
            o.u32(count);
            size_t offset = 4 + 4 * size_t(count);
            for (const auto& e : f.entries) {
                o.u32(uint32_t(offset));
                offset += e.text.size();
            }
            for (const auto& e : f.entries) o.bytes(e.text);
        } else {
            o.bytes(raw);
        }
        o.put32(size_at, uint32_t(o.b.size() - body));
        pad16(o);
    }
    o.put32(0x12, uint32_t(o.b.size()));
    out = std::move(o.b);
    return true;
}

const Entry* find(const File& f, std::string_view label) {
    for (const auto& e : f.entries)
        if (e.label == label) return &e;
    return nullptr;
}

void set(File& f, const std::string& label, Bytes text, Bytes attribute) {
    if (f.atr_strings && attribute.empty()) attribute = encode_string("", f.big_endian, true);  // a string, if an empty one
    for (auto& e : f.entries)
        if (e.label == label) {
            e.text = std::move(text);
            if (f.atr_strings) e.attribute = std::move(attribute);
            return;
        }
    Entry e;
    e.label = label;
    e.text = std::move(text);
    if (f.atr_strings) e.attribute = std::move(attribute);
    e.bucket = label_hash(label, f.groups);
    f.label_order.push_back(f.entries.size());
    f.entries.push_back(std::move(e));
}

File make(const std::string& name, bool big_endian) {
    File f;
    f.big_endian = big_endian;
    f.groups = 1;  // set by the caller once its entries are known (rehashed)
    f.atr1 = true;
    // as the game's own: EventFlowMsg files store the speaker of each text, the others no attributes
    f.atr_strings = name.rfind("EventFlowMsg/", 0) == 0;
    f.atr_size = f.atr_strings ? 4 : 0;
    f.sections = {{"LBL1", {}}, {"ATR1", {}}, {"TXT2", {}}};
    return f;
}

bool encode(const json::Value& contents, bool big_endian, Bytes& text, std::string* why) {
    if (contents.type != json::Value::Array) return fail(why, "texts: an entry's contents are not a list");
    Out o{big_endian, {}};
    Encoder e{o, why};
    for (const auto& part : contents.array) {
        if (part.type != json::Value::Object) return fail(why, "texts: a part is not an object");
        if (part.object.count("text")) {
            put_string(o, part.get("text").string());
        } else if (part.object.count("control")) {
            if (!e.control(part.get("control"))) return false;
        } else {
            return fail(why, "texts: a part is neither text nor a control");
        }
    }
    o.u16(0);
    text = std::move(o.b);
    return true;
}

Bytes encode_string(std::string_view utf8, bool big_endian, bool terminate) {
    Out o{big_endian, {}};
    put_string(o, utf8);
    if (terminate) o.u16(0);
    return std::move(o.b);
}

bool decode(const Bytes& text, bool big_endian, json::Value& contents, std::string* why) {
    if (text.size() % 2) return fail(why, "texts: odd text length");
    std::vector<uint16_t> u(text.size() / 2);
    for (size_t k = 0; k < u.size(); k++)
        u[k] = big_endian ? uint16_t(text[2 * k] << 8 | text[2 * k + 1]) : uint16_t(text[2 * k] | text[2 * k + 1] << 8);
    contents = json::Value();
    contents.type = json::Value::Array;
    Decoder dec{u, text, big_endian, why};
    std::vector<uint16_t> run;
    auto flush = [&] {
        std::vector<uint16_t> kept;
        for (uint16_t c : run)
            if (c) kept.push_back(c);
        run.clear();
        if (kept.empty()) return true;
        std::string s;
        if (!utf16_string(kept, 0, kept.size(), s)) return fail(why, "texts: bad UTF-16");
        json::Value part;
        part["text"] = s;
        contents.array.push_back(std::move(part));
        return true;
    };
    while (dec.p < u.size()) {
        const uint16_t c = u[dec.p++];
        if (c != 0x0E) {
            run.push_back(c);
            continue;
        }
        if (!flush()) return false;
        json::Value control;
        if (!dec.control(control)) return false;
        json::Value part;
        part["control"] = std::move(control);
        contents.array.push_back(std::move(part));
    }
    return flush();
}

}  // namespace mods::msbt
