// BotW (Wii U) file formats for the content mod loader (content_merge.cpp): Yaz0 compression, SARC
// archives (the .pack files and the archives nested in them) and the resource size table
// (System/Resource/ResourceSizeTable.product.srsizetable). Readers are bounds-checked; the writers
// produce big-endian data, the Wii U byte order.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mods::formats {

using Bytes = std::vector<uint8_t>;

inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

// ---- Yaz0
bool is_yaz0(const uint8_t* d, size_t n);           // magic and a whole header
uint32_t yaz0_size(const uint8_t* d, size_t n);     // decompressed size from the header (0: not Yaz0)
uint32_t yaz0_alignment(const uint8_t* d, size_t n);  // header word 8: alignment the decompressed data needs (often 0)
bool yaz0_decode(const uint8_t* d, size_t n, Bytes& out);  // false: corrupt stream
Bytes yaz0_encode(const uint8_t* d, size_t n, uint32_t alignment);
// the size of the data a file holds: its Yaz0 header's size, or n
inline size_t unpacked_size(const uint8_t* d, size_t n) { return is_yaz0(d, n) ? yaz0_size(d, n) : n; }

// ---- SARC
struct SarcEntry {
    std::string name;  // empty for an unnamed entry (none in BotW's packs)
    uint32_t hash = 0;
    size_t offset = 0, size = 0;  // from the start of the archive
};
struct Sarc {
    bool big_endian = true;
    uint32_t key = 0x65;  // name hash multiplier
    size_t data_offset = 0;
    std::vector<SarcEntry> entries;  // archive order (by hash)
};
bool is_sarc(const uint8_t* d, size_t n);  // magic only
// `file_size` (0: n) bounds the files: with a larger one, `d` may hold just the header and name table
// (the first data_offset bytes), enough to list the names.
bool sarc_parse(const uint8_t* d, size_t n, Sarc& out, std::string* why = nullptr, size_t file_size = 0);
uint32_t sarc_hash(std::string_view name, uint32_t key = 0x65);
struct SarcInput {
    std::string name;
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t alignment = 4;  // a power of two; the file starts at a multiple of it from the archive start
};
// A big-endian archive of `files` (any order; written in hash order). False if two names share a hash.
bool sarc_write(std::vector<SarcInput> files, uint32_t key, Bytes& out, std::string* why = nullptr);
// the largest power of two (4 to 0x2000) that divides `offset`: the alignment a file of a game archive
// may rely on (BotW's packs place files at 4- to 256-byte boundaries)
uint32_t natural_alignment(size_t offset);
// the alignment new data asks for: a Wii U BFRES header's (word 0x10), else 8
uint32_t data_alignment(const uint8_t* d, size_t n);

// ---- resource size table: memory the game reserves for each resource, by CRC-32 of its name (names
// whose CRC collides are listed by name)
struct Rstb {
    std::map<uint32_t, uint32_t> crc;
    std::map<std::string, uint32_t> names;
    bool operator==(const Rstb& o) const { return crc == o.crc && names == o.names; }
};
// the decompressed table ("RSTB" header, big-endian); false for anything else (a Switch table is little-endian)
bool rstb_parse(const uint8_t* d, size_t n, Rstb& out, std::string* why = nullptr);
Bytes rstb_write(const Rstb& t);
uint32_t rstb_crc(std::string_view name);
bool rstb_get(const Rstb& t, const std::string& name, uint32_t* size);
void rstb_set(Rstb& t, const std::string& name, uint32_t size);
void rstb_erase(Rstb& t, const std::string& name);
// The name a resource has in the size table: compressed files by their uncompressed extension
// ("Actor/Pack/X.sbactorpack" -> "Actor/Pack/X.bactorpack"; ".sarc" is not a compressed extension).
std::string resource_name(const std::string& path);
// The size-table entry for a resource that grew from `vanilla_size` to `new_size` bytes (unpacked), from
// its vanilla entry: simple resources keep their fixed overhead; for resources with a large overhead
// (models, parameters, animations) the growth counts at the entry's ratio to the size, plus a quarter.
uint32_t grown_entry(uint32_t vanilla_entry, uint64_t vanilla_size, uint64_t new_size);

}  // namespace mods::formats
