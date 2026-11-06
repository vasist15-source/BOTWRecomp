// AAMP parameter archives (agl::utl), the format of BotW's actor parameters (.bphysics, .bphyssb, .bxml,
// .bgparamlist, .baslist, ...) and of BCML's merge data (logs/deepmerge.aamp, logs/aslist.aamp in a .bnp).
// Version 2, little-endian, as BotW ships them. A file is kept as a tree of hashed names (CRC-32) with each
// parameter's type and raw value, so what isn't edited is written back the same.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mods::aamp {

enum Type : uint8_t {
    kBool, kF32, kInt, kVec2, kVec3, kVec4, kColor, kString32, kString64, kCurve1, kCurve2, kCurve3, kCurve4,
    kBufferInt, kBufferF32, kString256, kQuat, kU32, kBufferU32, kBufferBinary, kStringRef,
};
struct Param {
    uint32_t name = 0;
    uint8_t type = kInt;
    std::vector<uint8_t> data;  // the value's bytes; strings without their NUL; buffers: just the elements
    bool operator==(const Param& o) const { return name == o.name && type == o.type && data == o.data; }
};
struct Object {
    uint32_t name = 0;
    std::vector<Param> params;
    bool operator==(const Object& o) const { return name == o.name && params == o.params; }
};
struct List {
    uint32_t name = 0;
    std::vector<List> lists;
    std::vector<Object> objects;
    bool operator==(const List& o) const { return name == o.name && lists == o.lists && objects == o.objects; }
};
struct File {
    uint32_t flags = 3, pio_version = 0;
    std::string type = "xml";
    List root;
};

uint32_t hash(std::string_view name);  // CRC-32 of the name
bool is_string(uint8_t type);
std::string text(const Param& p);  // a string parameter's value
bool parse(const uint8_t* d, size_t n, File& out, std::string* why = nullptr);
bool write(const File& f, std::vector<uint8_t>& out, std::string* why = nullptr);

const Object* object(const List& l, uint32_t name);
const List* list(const List& l, uint32_t name);
const Param* param(const Object& o, uint32_t name);
// BCML's deep merge: the diff's parameters, objects and lists go into `target` (by name; new ones last)
void merge(List& target, const List& diff);
// BCML's AS list merge (.baslist): AddReses by their Anim, ASDefines by Name, CFDefines by their CFPre's Name
// (new ones get the next AddRes_N / ASDefine_N / CFDefine_N); anything else as merge()
void merge_aslist(List& target, const List& diff);

}  // namespace mods::aamp
