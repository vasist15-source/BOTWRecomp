// AAMP parameter archives: see aamp.h.
//   header (0x30 bytes): "AAMP", version 2, flags (1 little-endian, 2 UTF-8), file size, pio version, offset
//   of the root list after the header (= the type string's size, "xml"), list/object/parameter counts, data
//   and string section sizes, 0
//   list (12 bytes): name, child lists (u16 offset / 4 from the list, u16 count), objects (offset, count)
//   object (8 bytes): name, parameters (u16 offset / 4 from the object, u16 count)
//   parameter (8 bytes): name, value offset / 4 from the parameter (24 bits) | type << 24; buffers have their
//   element count in the word before the value, strings end with a NUL
// Equal values and strings are stored once, as in the game's own files: a written file is about the size of
// the one it was read from (its size-table entry counts on that).
#include "aamp.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <map>

namespace mods::aamp {
namespace {

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i));
}
bool fail(std::string* why, const char* text) {
    if (why) *why = text;
    return false;
}
size_t fixed_size(uint8_t t) {
    switch (t) {
    case kBool: case kF32: case kInt: case kU32: return 4;
    case kVec2: return 8;
    case kVec3: return 12;
    case kVec4: case kColor: case kQuat: return 16;
    case kCurve1: case kCurve2: case kCurve3: case kCurve4: return size_t(0x80) * (t - kCurve1 + 1);
    default: return 0;
    }
}
bool is_buffer(uint8_t t) { return t == kBufferInt || t == kBufferF32 || t == kBufferU32 || t == kBufferBinary; }
size_t align4(size_t v) { return (v + 3) & ~size_t(3); }

bool read_list(const uint8_t* d, size_t n, size_t at, List& out, int depth, std::string* why) {
    if (depth > 64) return fail(why, "parameter lists nested too deep");
    if (at + 12 > n) return fail(why, "parameter list out of range");
    out.name = le32(d + at);
    const size_t lists = at + size_t(le16(d + at + 4)) * 4, list_count = le16(d + at + 6);
    const size_t objects = at + size_t(le16(d + at + 8)) * 4, object_count = le16(d + at + 10);
    out.lists.resize(list_count);
    for (size_t i = 0; i < list_count; i++)
        if (!read_list(d, n, lists + 12 * i, out.lists[i], depth + 1, why)) return false;
    out.objects.resize(object_count);
    for (size_t i = 0; i < object_count; i++) {
        const size_t o = objects + 8 * i;
        if (o + 8 > n) return fail(why, "parameter object out of range");
        Object& obj = out.objects[i];
        obj.name = le32(d + o);
        const size_t params = o + size_t(le16(d + o + 4)) * 4, param_count = le16(d + o + 6);
        obj.params.resize(param_count);
        for (size_t k = 0; k < param_count; k++) {
            const size_t p = params + 8 * k;
            if (p + 8 > n) return fail(why, "parameter out of range");
            Param& prm = obj.params[k];
            prm.name = le32(d + p);
            const uint32_t word = le32(d + p + 4);
            prm.type = uint8_t(word >> 24);
            const size_t value = p + size_t(word & 0xFFFFFF) * 4;
            if (value > n) return fail(why, "parameter value out of range");
            if (is_string(prm.type)) {
                const void* nul = memchr(d + value, 0, n - value);
                if (!nul) return fail(why, "unterminated string parameter");
                prm.data.assign(d + value, static_cast<const uint8_t*>(nul));
            } else if (is_buffer(prm.type)) {
                if (value < 4) return fail(why, "buffer parameter out of range");
                const uint64_t bytes = uint64_t(le32(d + value - 4)) * (prm.type == kBufferBinary ? 1 : 4);
                if (value + bytes > n) return fail(why, "buffer parameter out of range");
                prm.data.assign(d + value, d + value + bytes);
            } else {
                const size_t size = fixed_size(prm.type);
                if (!size) return fail(why, "unknown parameter type");
                if (value + size > n) return fail(why, "parameter value out of range");
                prm.data.assign(d + value, d + value + size);
            }
        }
    }
    return true;
}

}  // namespace

uint32_t hash(std::string_view name) { return uint32_t(crc32(0, reinterpret_cast<const Bytef*>(name.data()), uInt(name.size()))); }
bool is_string(uint8_t t) { return t == kString32 || t == kString64 || t == kString256 || t == kStringRef; }
std::string text(const Param& p) { return std::string(p.data.begin(), p.data.end()); }

bool parse(const uint8_t* d, size_t n, File& out, std::string* why) {
    if (n < 0x30 || memcmp(d, "AAMP", 4)) return fail(why, "not a parameter archive (AAMP)");
    if (le32(d + 4) != 2) return fail(why, "unsupported AAMP version");
    out = {};
    out.flags = le32(d + 8);
    if (!(out.flags & 1)) return fail(why, "big-endian AAMP is not supported");
    out.pio_version = le32(d + 0x10);
    const size_t root = 0x30 + size_t(le32(d + 0x14));
    if (root + 12 > n) return fail(why, "AAMP root list out of range");
    const void* nul = memchr(d + 0x30, 0, root - 0x30);
    out.type.assign(reinterpret_cast<const char*>(d + 0x30), nul ? static_cast<const uint8_t*>(nul) - (d + 0x30) : root - 0x30);
    return read_list(d, n, root, out.root, 0, why);
}

bool write(const File& f, std::vector<uint8_t>& out, std::string* why) {
    // lists breadth first (each list's children together), then objects, parameters, values and strings
    std::vector<const List*> lists{&f.root};
    for (size_t i = 0; i < lists.size(); i++)
        for (const auto& c : lists[i]->lists) lists.push_back(&c);
    std::vector<size_t> first_list(lists.size()), first_object(lists.size());
    std::vector<const Object*> objects;
    for (size_t i = 0, next = 1; i < lists.size(); i++) {
        first_list[i] = next;
        next += lists[i]->lists.size();
        first_object[i] = objects.size();
        for (const auto& o : lists[i]->objects) objects.push_back(&o);
    }
    std::vector<size_t> first_param(objects.size());
    std::vector<const Param*> params;
    for (size_t j = 0; j < objects.size(); j++) {
        first_param[j] = params.size();
        for (const auto& p : objects[j]->params) params.push_back(&p);
    }
    const size_t type_size = align4(f.type.size() + 1);
    const size_t lists_at = 0x30 + type_size, objects_at = lists_at + 12 * lists.size(), params_at = objects_at + 8 * objects.size();
    const size_t data_at = params_at + 8 * params.size();
    std::vector<uint8_t> data, strings;
    std::vector<size_t> value(params.size());
    std::map<std::vector<uint8_t>, size_t> stored;  // a value block (a buffer with its count) -> where it is
    for (size_t k = 0; k < params.size(); k++) {
        const Param& p = *params[k];
        if (is_string(p.type)) continue;
        std::vector<uint8_t> block;
        size_t skip = 0;  // a buffer's value starts after its count
        if (is_buffer(p.type)) {
            const size_t elem = p.type == kBufferBinary ? 1 : 4;
            if (p.data.size() % elem) return fail(why, "buffer parameter of a partial element");
            block.resize(4);
            put32(block.data(), uint32_t(p.data.size() / elem));
            skip = 4;
        } else if (p.data.size() != fixed_size(p.type) || !fixed_size(p.type)) {
            return fail(why, "parameter value of the wrong size");
        }
        block.insert(block.end(), p.data.begin(), p.data.end());
        auto [it, fresh] = stored.emplace(std::move(block), data.size());
        if (fresh) {
            data.insert(data.end(), it->first.begin(), it->first.end());
            data.resize(align4(data.size()), 0);
        }
        value[k] = data_at + it->second + skip;
    }
    const size_t strings_at = data_at + data.size();
    std::map<std::vector<uint8_t>, size_t> texts;
    for (size_t k = 0; k < params.size(); k++) {
        const Param& p = *params[k];
        if (!is_string(p.type)) continue;
        if (std::find(p.data.begin(), p.data.end(), 0) != p.data.end()) return fail(why, "string parameter with a NUL");
        auto [it, fresh] = texts.emplace(p.data, strings.size());
        if (fresh) {
            strings.insert(strings.end(), p.data.begin(), p.data.end());
            strings.push_back(0);
            strings.resize(align4(strings.size()), 0);
        }
        value[k] = strings_at + it->second;
    }
    const size_t total = strings_at + strings.size();
    if (total > 0xFFFFFFFFull) return fail(why, "parameter archive larger than 4 GB");
    out.assign(total, 0);
    uint8_t* b = out.data();
    memcpy(b, "AAMP", 4);
    put32(b + 4, 2);
    put32(b + 8, f.flags);
    put32(b + 0xC, uint32_t(total));
    put32(b + 0x10, f.pio_version);
    put32(b + 0x14, uint32_t(type_size));
    put32(b + 0x18, uint32_t(lists.size()));
    put32(b + 0x1C, uint32_t(objects.size()));
    put32(b + 0x20, uint32_t(params.size()));
    put32(b + 0x24, uint32_t(data.size()));
    put32(b + 0x28, uint32_t(strings.size()));
    memcpy(b + 0x30, f.type.data(), f.type.size());
    auto rel16 = [&](size_t from, size_t to, uint16_t& v) {  // a forward offset / 4 that fits 16 bits
        const size_t r = (to - from) / 4;
        if (to < from || r > 0xFFFF) return false;
        v = uint16_t(r);
        return true;
    };
    for (size_t i = 0; i < lists.size(); i++) {
        const List& l = *lists[i];
        const size_t at = lists_at + 12 * i;
        uint16_t lo = 0, oo = 0;
        if (l.lists.size() > 0xFFFF || l.objects.size() > 0xFFFF) return fail(why, "too many entries in a parameter list");
        if (!l.lists.empty() && !rel16(at, lists_at + 12 * first_list[i], lo)) return fail(why, "parameter archive too large");
        if (!l.objects.empty() && !rel16(at, objects_at + 8 * first_object[i], oo)) return fail(why, "parameter archive too large");
        put32(b + at, l.name);
        put16(b + at + 4, lo);
        put16(b + at + 6, uint16_t(l.lists.size()));
        put16(b + at + 8, oo);
        put16(b + at + 10, uint16_t(l.objects.size()));
    }
    for (size_t j = 0; j < objects.size(); j++) {
        const Object& o = *objects[j];
        const size_t at = objects_at + 8 * j;
        uint16_t po = 0;
        if (o.params.size() > 0xFFFF) return fail(why, "too many parameters in an object");
        if (!o.params.empty() && !rel16(at, params_at + 8 * first_param[j], po)) return fail(why, "parameter archive too large");
        put32(b + at, o.name);
        put16(b + at + 4, po);
        put16(b + at + 6, uint16_t(o.params.size()));
    }
    for (size_t k = 0; k < params.size(); k++) {
        const size_t at = params_at + 8 * k, r = (value[k] - at) / 4;
        if (r > 0xFFFFFF) return fail(why, "parameter archive too large");
        put32(b + at, params[k]->name);
        put32(b + at + 4, uint32_t(r) | uint32_t(params[k]->type) << 24);
    }
    if (!data.empty()) memcpy(b + data_at, data.data(), data.size());
    if (!strings.empty()) memcpy(b + strings_at, strings.data(), strings.size());
    return true;
}

const Object* object(const List& l, uint32_t name) {
    for (const auto& o : l.objects)
        if (o.name == name) return &o;
    return nullptr;
}
const List* list(const List& l, uint32_t name) {
    for (const auto& c : l.lists)
        if (c.name == name) return &c;
    return nullptr;
}
const Param* param(const Object& o, uint32_t name) {
    for (const auto& p : o.params)
        if (p.name == name) return &p;
    return nullptr;
}

namespace {
List* list_of(List& l, uint32_t name) { return const_cast<List*>(list(l, name)); }
Object* object_of(List& l, uint32_t name) { return const_cast<Object*>(object(l, name)); }
void merge_object(Object& target, const Object& diff) {
    for (const auto& p : diff.params) {
        auto it = std::find_if(target.params.begin(), target.params.end(), [&](const Param& q) { return q.name == p.name; });
        if (it != target.params.end()) *it = p;
        else target.params.push_back(p);
    }
}
const Param* name_of_define(const List& def) {  // a CFDefine's CFPre Name
    const Object* pre = object(def, hash("CFPre"));
    return pre ? param(*pre, hash("Name")) : nullptr;
}
}  // namespace

void merge(List& target, const List& diff) {
    for (const auto& dl : diff.lists) {
        if (List* t = list_of(target, dl.name)) merge(*t, dl);
        else target.lists.push_back(dl);
    }
    for (const auto& dobj : diff.objects) {
        if (Object* t = object_of(target, dobj.name)) merge_object(*t, dobj);
        else target.objects.push_back(dobj);
    }
}

void merge_aslist(List& target, const List& diff) {
    const uint32_t add_reses = hash("AddReses"), as_defines = hash("ASDefines"), cf_defines = hash("CFDefines");
    const uint32_t anim = hash("Anim"), name = hash("Name");
    for (const auto& dl : diff.lists) {
        List* t = list_of(target, dl.name);
        if (!t) {
            target.lists.push_back(dl);
            continue;
        }
        if (dl.name == add_reses) {  // animation archives the actor loads: a set
            for (const auto& o : dl.objects) {
                const Param* a = param(o, anim);
                if (!a) continue;
                const bool have = std::any_of(t->objects.begin(), t->objects.end(), [&](const Object& x) {
                    const Param* b = param(x, anim);
                    return b && b->data == a->data;
                });
                if (have) continue;
                Object added = o;
                added.name = hash("AddRes_" + std::to_string(t->objects.size()));
                t->objects.push_back(std::move(added));
            }
        } else if (dl.name == as_defines) {  // animation sequences by Name
            for (const auto& o : dl.objects) {
                const Param* n = param(o, name);
                Object* same = nullptr;
                if (n)
                    for (auto& x : t->objects)
                        if (const Param* m = param(x, name); m && m->data == n->data) {
                            same = &x;
                            break;
                        }
                if (!n) {
                    if (Object* key = object_of(*t, o.name)) merge_object(*key, o);
                    else t->objects.push_back(o);
                } else if (same) {
                    merge_object(*same, o);
                } else {
                    Object added = o;
                    added.name = hash("ASDefine_" + std::to_string(t->objects.size()));
                    t->objects.push_back(std::move(added));
                }
            }
        } else if (dl.name == cf_defines) {  // transitions by their CFPre's Name
            for (const auto& def : dl.lists) {
                const Param* n = name_of_define(def);
                List* same = nullptr;
                if (n)
                    for (auto& x : t->lists)
                        if (const Param* m = name_of_define(x); m && m->data == n->data) {
                            same = &x;
                            break;
                        }
                if (same) {
                    merge(*same, def);
                } else if (!n) {
                    if (List* key = list_of(*t, def.name)) merge(*key, def);
                    else t->lists.push_back(def);
                } else {
                    List added = def;
                    added.name = hash("CFDefine_" + std::to_string(t->lists.size()));
                    t->lists.push_back(std::move(added));
                }
            }
        } else {
            merge(*t, dl);
        }
    }
    for (const auto& dobj : diff.objects) {
        if (Object* t = object_of(target, dobj.name)) merge_object(*t, dobj);
        else target.objects.push_back(dobj);
    }
}

}  // namespace mods::aamp
