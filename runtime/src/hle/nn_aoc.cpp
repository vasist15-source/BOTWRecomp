// nn_aoc: add-on content (DLC). One AOC title is offered when --aoc names an extracted DLC
// (code/, content/, meta/); the game opens it as /vol/aoc<title id> (mapped by fs.cpp). Every item is
// reported as purchased, as Cemu does without a ticket. Struct layouts follow Cemu's nn_aoc.
#include <cstdio>
#include <cstring>
#include <string>

#include "../runtime.h"

namespace {

enum : uint32_t { AOC_OK = 0, AOC_INVALID = (uint32_t)-1 };

// AOCTitle (0x68 bytes): u64 titleId, u32 groupId, u16 titleVersion, char path[88], 2 bytes pad
constexpr uint32_t kTitleSize = 0x68;

// the DLC's title id, from <aoc>/meta/meta.xml (<title_id ...>0005000C101C9400</title_id>); 0 if none
uint64_t aoc_title_id() {
    static uint64_t id = [] {
        if (config::aoc_dir.empty()) return (uint64_t)0;
        FILE* f = fopen((config::aoc_dir + "/meta/meta.xml").c_str(), "r");
        if (!f) return (uint64_t)0;
        char line[512];
        uint64_t v = 0;
        while (fgets(line, sizeof line, f))
            if (const char* t = strstr(line, "<title_id")) {
                if (const char* gt = strchr(t, '>')) v = strtoull(gt + 1, nullptr, 16);
                break;
            }
        fclose(f);
        LOG("[aoc] %s: title %016llX", config::aoc_dir.c_str(), (unsigned long long)v);
        return v;
    }();
    return id;
}

std::string aoc_path(uint64_t id) {
    char buf[40];
    snprintf(buf, sizeof buf, "/vol/aoc%016llx", (unsigned long long)id);
    return buf;
}

}  // namespace

HLE(nn_aoc, AOC_Initialize) { ret(c, AOC_OK); }
HLE(nn_aoc, AOC_Finalize) { ret(c, AOC_OK); }
HLE(nn_aoc, AOC_DebugRealDeviceAccess) { ret(c, AOC_OK); }
HLE(nn_aoc, AOC_CalculateWorkBufferSize) {  // (count) -> bytes
    uint32_t n = std::min<uint32_t>(arg(c, 0), 256);
    ret(c, 0x80 + n * 0x61);
}
// (u32* count, AOCTitle* list, maxCount, workBuffer, workBufferSize)
HLE(nn_aoc, AOC_ListTitle) {
    uint32_t count_out = arg(c, 0), list = arg(c, 1), max = arg(c, 2), n = 0;
    if (uint64_t id = aoc_title_id(); id && max > 0 && list) {
        memset(mem::ptr(list), 0, kTitleSize);
        st32(list + 0, (uint32_t)(id >> 32));
        st32(list + 4, (uint32_t)id);
        mem::write_cstr(list + 0xE, aoc_path(id).c_str(), 88);
        n = 1;
    }
    if (count_out) st32(count_out, n);
    LOG("[aoc] AOC_ListTitle -> %u title(s)", n);
    ret(c, AOC_OK);
}
// (char* pathOut, AOCTitle* title, workBuffer, workBufferSize)
HLE(nn_aoc, AOC_OpenTitle) {
    char path[88] = {};
    memcpy(path, mem::ptr(arg(c, 1) + 0xE), sizeof path - 1);
    mem::write_cstr(arg(c, 0), path, 88);
    LOG("[aoc] AOC_OpenTitle %s", path);
    ret(c, AOC_OK);
}
HLE(nn_aoc, AOC_CloseTitle) { ret(c, AOC_OK); }
// (u32* purchasedOut, u64 titleId (r5:r6), u16* entryIds, entryCount, workBuffer, workBufferSize)
HLE(nn_aoc, AOC_GetPurchaseInfo) {
    uint32_t out = arg(c, 0), count = c->r[8];
    for (uint32_t i = 0; i < count && out; i++) st32(out + 4 * i, 1);
    ret(c, AOC_OK);
}
