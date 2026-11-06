// BotW (Wii U) file formats for the content mod loader: see botw_formats.h.
#include "botw_formats.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mods::formats {
namespace {
bool fail(std::string* why, const char* text) {
    if (why) *why = text;
    return false;
}
size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }
}  // namespace

// ---------------------------------------------------------------- Yaz0
bool is_yaz0(const uint8_t* d, size_t n) { return n >= 16 && !memcmp(d, "Yaz0", 4); }
uint32_t yaz0_size(const uint8_t* d, size_t n) { return is_yaz0(d, n) ? be32(d + 4) : 0; }
uint32_t yaz0_alignment(const uint8_t* d, size_t n) { return is_yaz0(d, n) ? be32(d + 8) : 0; }

bool yaz0_decode(const uint8_t* src, size_t n, Bytes& out) {
    if (!is_yaz0(src, n)) return false;
    const size_t size = be32(src + 4);
    out.resize(size);
    uint8_t* dst = out.data();
    size_t o = 0, i = 16;
    while (o < size) {
        if (i >= n) return false;
        uint8_t code = src[i++];
        for (int bit = 0; bit < 8 && o < size; bit++, code <<= 1) {
            if (code & 0x80) {
                if (i >= n) return false;
                dst[o++] = src[i++];
                continue;
            }
            if (i + 2 > n) return false;
            const uint8_t b1 = src[i], b2 = src[i + 1];
            i += 2;
            const size_t dist = (size_t(b1 & 15) << 8 | b2) + 1;
            size_t len = b1 >> 4;
            if (len == 0) {
                if (i >= n) return false;
                len = size_t(src[i++]) + 0x12;
            } else {
                len += 2;
            }
            if (dist > o) return false;
            len = std::min(len, size - o);
            uint8_t* p = dst + o;
            const uint8_t* q = p - dist;
            for (size_t k = 0; k < len; k++) p[k] = q[k];  // may overlap (dist < len): byte by byte
            o += len;
        }
    }
    return true;
}

// Greedy LZ77 over hash chains: about the ratio of Nintendo's own encoder, fast enough for the archives
// the loader rebuilds (actor packs, the size table).
Bytes yaz0_encode(const uint8_t* src, size_t n, uint32_t alignment) {
    Bytes out;
    out.reserve(16 + n + n / 8 + 16);
    out.resize(16);
    memcpy(out.data(), "Yaz0", 4);
    put32(out.data() + 4, uint32_t(n));
    put32(out.data() + 8, alignment);
    put32(out.data() + 12, 0);
    constexpr size_t kWindow = 0x1000, kMax = 0x111, kChain = 48;
    constexpr int kHashBits = 15;
    std::vector<int32_t> head(size_t(1) << kHashBits, -1), prev(n, -1);
    auto hash = [&](size_t i) {
        const uint32_t v = uint32_t(src[i]) << 16 | uint32_t(src[i + 1]) << 8 | src[i + 2];
        return (v * 2654435761u) >> (32 - kHashBits);
    };
    auto insert = [&](size_t i) {
        if (i + 3 > n) return;
        const uint32_t h = hash(i);
        prev[i] = head[h];
        head[h] = int32_t(i);
    };
    size_t i = 0;
    while (i < n) {
        const size_t flag_at = out.size();
        out.push_back(0);
        uint8_t flags = 0;
        for (int bit = 0; bit < 8 && i < n; bit++) {
            size_t best = 0, dist = 0;
            if (i + 3 <= n) {
                const size_t limit = std::min(kMax, n - i);
                int32_t cand = head[hash(i)];
                for (size_t chain = 0; cand >= 0 && i - size_t(cand) <= kWindow && chain < kChain; chain++) {
                    const uint8_t* a = src + cand;
                    const uint8_t* b = src + i;
                    if (a[best] == b[best]) {  // only a longer match counts
                        size_t l = 0;
                        while (l < limit && a[l] == b[l]) l++;
                        if (l > best) {
                            best = l;
                            dist = i - size_t(cand);
                            if (l == limit) break;
                        }
                    }
                    cand = prev[size_t(cand)];
                }
            }
            if (best >= 3) {
                const size_t d = dist - 1;
                if (best >= 0x12) {
                    out.push_back(uint8_t(d >> 8));
                    out.push_back(uint8_t(d));
                    out.push_back(uint8_t(best - 0x12));
                } else {
                    out.push_back(uint8_t((best - 2) << 4 | d >> 8));
                    out.push_back(uint8_t(d));
                }
                for (size_t k = 0; k < best; k++) insert(i + k);
                i += best;
            } else {
                flags |= uint8_t(0x80 >> bit);
                out.push_back(src[i]);
                insert(i);
                i++;
            }
        }
        out[flag_at] = flags;
    }
    return out;
}

// ---------------------------------------------------------------- SARC
bool is_sarc(const uint8_t* d, size_t n) { return n >= 0x20 && !memcmp(d, "SARC", 4); }

uint32_t sarc_hash(std::string_view name, uint32_t key) {
    uint32_t h = 0;
    for (char c : name) h = h * key + uint8_t(c);
    return h;
}

bool sarc_parse(const uint8_t* d, size_t n, Sarc& out, std::string* why, size_t file_size) {
    if (!is_sarc(d, n)) return fail(why, "not a SARC archive");
    if (file_size < n) file_size = n;
    const bool big = d[6] == 0xFE && d[7] == 0xFF, little = d[6] == 0xFF && d[7] == 0xFE;
    if (!big && !little) return fail(why, "SARC archive without a byte order mark");
    auto r16 = [&](size_t at) { return big ? be16(d + at) : uint16_t(d[at] | d[at + 1] << 8); };
    auto r32 = [&](size_t at) {
        return big ? be32(d + at) : uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24;
    };
    if (r16(4) != 0x14 || memcmp(d + 0x14, "SFAT", 4) || r16(0x18) != 0xC) return fail(why, "malformed SARC header");
    out = {};
    out.big_endian = big;
    out.data_offset = r32(0x0C);
    const size_t count = r16(0x1A);
    out.key = r32(0x1C);
    const size_t nodes = 0x20, sfnt = nodes + count * 16, names = sfnt + 8;
    if (names > n || memcmp(d + sfnt, "SFNT", 4)) return fail(why, "malformed SARC name table");
    if (out.data_offset < names || out.data_offset > n || out.data_offset > file_size) return fail(why, "SARC data offset out of range");
    out.entries.reserve(count);
    for (size_t k = 0; k < count; k++) {
        const size_t e = nodes + k * 16;
        SarcEntry entry;
        entry.hash = r32(e);
        const uint32_t attr = r32(e + 4), begin = r32(e + 8), end = r32(e + 12);
        if (end < begin || out.data_offset + size_t(end) > file_size) return fail(why, "SARC file out of range");
        entry.offset = out.data_offset + begin;
        entry.size = end - begin;
        if (attr >> 24) {
            const size_t at = names + size_t(attr & 0xFFFFFF) * 4;
            if (at >= out.data_offset) return fail(why, "SARC file name out of range");
            const void* nul = memchr(d + at, 0, out.data_offset - at);
            if (!nul) return fail(why, "unterminated SARC file name");
            entry.name.assign(reinterpret_cast<const char*>(d + at), static_cast<const uint8_t*>(nul) - (d + at));
        }
        out.entries.push_back(std::move(entry));
    }
    return true;
}

bool sarc_write(std::vector<SarcInput> files, uint32_t key, Bytes& out, std::string* why) {
    struct Item {
        SarcInput in;
        uint32_t hash;
        size_t name_at;
    };
    std::vector<Item> items;
    items.reserve(files.size());
    for (auto& f : files) items.push_back({std::move(f), 0, 0});
    for (auto& it : items) it.hash = sarc_hash(it.in.name, key);
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.hash < b.hash; });
    for (size_t k = 1; k < items.size(); k++)
        if (items[k].hash == items[k - 1].hash)
            return fail(why, items[k].in.name == items[k - 1].in.name ? "duplicate file name in archive" : "two file names with the same hash");
    if (items.size() > 0xFFFF) return fail(why, "too many files for one archive");
    size_t names = 0;
    for (auto& it : items) {
        it.name_at = names;
        names += align_up(it.in.name.size() + 1, 4);
    }
    const size_t header_end = 0x20 + items.size() * 16 + 8 + names;
    size_t max_align = 4;
    for (const auto& it : items) max_align = std::max<size_t>(max_align, it.in.alignment);
    const size_t data = align_up(header_end, max_align);
    size_t cursor = data;
    std::vector<size_t> at(items.size());
    for (size_t k = 0; k < items.size(); k++) {
        at[k] = align_up(cursor, std::max<size_t>(items[k].in.alignment, 1));
        cursor = at[k] + items[k].in.size;
    }
    if (cursor > 0xFFFFFFFFull) return fail(why, "archive larger than 4 GB");
    out.assign(cursor, 0);
    uint8_t* p = out.data();
    memcpy(p, "SARC", 4);
    put16(p + 4, 0x14);
    put16(p + 6, 0xFEFF);
    put32(p + 8, uint32_t(cursor));
    put32(p + 0xC, uint32_t(data));
    put16(p + 0x10, 0x0100);
    memcpy(p + 0x14, "SFAT", 4);
    put16(p + 0x18, 0xC);
    put16(p + 0x1A, uint16_t(items.size()));
    put32(p + 0x1C, key);
    const size_t sfnt = 0x20 + items.size() * 16;
    for (size_t k = 0; k < items.size(); k++) {
        uint8_t* e = p + 0x20 + k * 16;
        put32(e, items[k].hash);
        put32(e + 4, 0x01000000u | uint32_t(items[k].name_at / 4));
        put32(e + 8, uint32_t(at[k] - data));
        put32(e + 12, uint32_t(at[k] - data + items[k].in.size));
        memcpy(p + sfnt + 8 + items[k].name_at, items[k].in.name.data(), items[k].in.name.size());
        if (items[k].in.size) memcpy(p + at[k], items[k].in.data, items[k].in.size);
    }
    memcpy(p + sfnt, "SFNT", 4);
    put16(p + sfnt + 4, 8);
    return true;
}

uint32_t natural_alignment(size_t offset) {
    uint32_t a = 4;
    while (a < 0x2000 && offset % (size_t(a) * 2) == 0) a *= 2;
    return a;
}

uint32_t data_alignment(const uint8_t* d, size_t n) {
    if (n >= 0x14 && !memcmp(d, "FRES", 4)) {
        const uint32_t a = be32(d + 0x10);
        if (a && !(a & (a - 1)) && a <= 0x2000) return std::max<uint32_t>(a, 8);
    }
    return 8;
}

// ---------------------------------------------------------------- resource size table
bool rstb_parse(const uint8_t* d, size_t n, Rstb& out, std::string* why) {
    if (n < 12 || memcmp(d, "RSTB", 4)) return fail(why, "not a resource size table");
    const uint64_t crcs = be32(d + 4), names = be32(d + 8);
    if (12 + crcs * 8 + names * 132 != n) return fail(why, "resource size table of the wrong size (a Switch table?)");
    out = {};
    const uint8_t* p = d + 12;
    for (uint64_t k = 0; k < crcs; k++, p += 8) out.crc[be32(p)] = be32(p + 4);
    for (uint64_t k = 0; k < names; k++, p += 132) {
        const void* nul = memchr(p, 0, 128);
        const size_t len = nul ? static_cast<const uint8_t*>(nul) - p : 128;
        out.names[std::string(reinterpret_cast<const char*>(p), len)] = be32(p + 128);
    }
    return true;
}

Bytes rstb_write(const Rstb& t) {
    Bytes out(12 + t.crc.size() * 8 + t.names.size() * 132, 0);
    uint8_t* p = out.data();
    memcpy(p, "RSTB", 4);
    put32(p + 4, uint32_t(t.crc.size()));
    put32(p + 8, uint32_t(t.names.size()));
    p += 12;
    for (const auto& [crc, size] : t.crc) {  // std::map: ascending CRCs, as the game's binary search needs
        put32(p, crc);
        put32(p + 4, size);
        p += 8;
    }
    for (const auto& [name, size] : t.names) {
        memcpy(p, name.data(), std::min<size_t>(name.size(), 127));
        put32(p + 128, size);
        p += 132;
    }
    return out;
}

uint32_t rstb_crc(std::string_view name) {
    return uint32_t(crc32(0, reinterpret_cast<const Bytef*>(name.data()), uInt(name.size())));
}

bool rstb_get(const Rstb& t, const std::string& name, uint32_t* size) {
    if (auto it = t.names.find(name); it != t.names.end()) {
        *size = it->second;
        return true;
    }
    if (auto it = t.crc.find(rstb_crc(name)); it != t.crc.end()) {
        *size = it->second;
        return true;
    }
    return false;
}

void rstb_set(Rstb& t, const std::string& name, uint32_t size) {
    if (auto it = t.names.find(name); it != t.names.end()) it->second = size;
    else t.crc[rstb_crc(name)] = size;
}

void rstb_erase(Rstb& t, const std::string& name) {
    if (t.names.erase(name)) return;
    t.crc.erase(rstb_crc(name));
}

std::string resource_name(const std::string& path) {
    const size_t slash = path.rfind('/');
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path;
    const std::string ext = path.substr(dot);
    if (ext.size() > 2 && ext[1] == 's' && ext != ".sarc") return path.substr(0, dot + 1) + ext.substr(2);
    return path;
}

uint32_t grown_entry(uint32_t vanilla_entry, uint64_t vanilla_size, uint64_t new_size) {
    if (new_size <= vanilla_size) return vanilla_entry;
    const uint64_t overhead = vanilla_entry > vanilla_size ? vanilla_entry - vanilla_size : 0;
    uint64_t entry;
    if (overhead <= std::max<uint64_t>(0x1000, vanilla_size / 4)) {
        entry = ((new_size + 31) & ~uint64_t(31)) + overhead;
    } else {  // the growth at the entry's ratio to the size, and a quarter more
        const double ratio = double(vanilla_entry) / double(std::max<uint64_t>(vanilla_size, 1));
        entry = vanilla_entry + uint64_t(std::ceil(double(new_size - vanilla_size) * ratio * 1.25));
    }
    entry = std::max<uint64_t>(entry, vanilla_entry);
    return uint32_t(std::min<uint64_t>(entry, 0xFFFFFFFFu));
}

}  // namespace mods::formats
