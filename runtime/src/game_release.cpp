// The installed release of the game: USA or Europe (game_release.h).
#include "game_release.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "runtime.h"

namespace game_release {
namespace {

namespace fs = std::filesystem;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

// a metadata file (a few KB); "" if it isn't there
std::string read_small(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::string s;
    char buf[4096];
    while (f && s.size() < (1u << 20)) {
        f.read(buf, sizeof buf);
        s.append(buf, (size_t)f.gcount());
    }
    return s;
}

// the file names in <root>/content/Pack
std::vector<std::string> pack_names(const std::string& root) {
    std::vector<std::string> out;
    if (root.empty()) return out;
    std::error_code ec;
    for (fs::directory_iterator it(fs::path(root) / "content" / "Pack", ec), end; !ec && it != end; it.increment(ec))
        out.push_back(it->path().filename().string());
    return out;
}

}  // namespace

const char* name(int region) {
    return region == kUsa ? "USA" : region == kEurope ? "Europe" : region == kJapan ? "Japan" : "?";
}

uint32_t country(int region, int language) {
    if (region == kJapan) return 1;   // Japan
    if (region != kEurope) return 49;  // United States (French and Spanish too: one console country)
    switch (language) {  // Wii U console languages: 2 fr, 3 de, 4 it, 5 es, 8 nl, 9 pt, 10 ru
    case 2: return 77;    // France
    case 3: return 78;    // Germany
    case 4: return 83;    // Italy
    case 5: return 105;   // Spain
    case 8: return 94;    // Netherlands
    case 9: return 98;    // Portugal
    case 10: return 100;  // Russia
    default: return 110;  // United Kingdom
    }
}

std::string tag_text(const std::string& xml, const char* tag) {
    const std::string open = std::string("<") + tag;
    for (size_t at = xml.find(open); at != std::string::npos; at = xml.find(open, at + 1)) {
        const size_t after = at + open.size();
        if (after < xml.size() && xml[after] != '>' && !isspace((unsigned char)xml[after])) continue;  // a longer name
        const size_t gt = xml.find('>', after), end = gt == std::string::npos ? gt : xml.find('<', gt);
        if (end == std::string::npos) return {};
        size_t a = gt + 1, b = end;
        while (a < b && isspace((unsigned char)xml[a])) a++;
        while (b > a && isspace((unsigned char)xml[b - 1])) b--;
        return xml.substr(a, b - a);
    }
    return {};
}

std::string title_of(const std::string& xml) {
    std::string t = tag_text(xml, "title_id");
    if (t.size() != 16) return {};
    for (char& ch : t) {
        if (!isxdigit((unsigned char)ch)) return {};
        ch = (char)toupper((unsigned char)ch);
    }
    if (t.compare(0, 8, "0005000E") == 0) t.replace(0, 8, "00050000");  // an update: its base game
    return t;
}

int region_of_title(const std::string& title_id) {
    std::string t = title_id;
    for (char& ch : t) ch = (char)toupper((unsigned char)ch);
    if (t.size() != 16 || t.compare(0, 8, "00050000") != 0) return kNone;
    const std::string low = t.substr(8);
    return low == "101C9400" ? kUsa : low == "101C9500" ? kEurope : low == "101C9300" ? kJapan : kNone;
}

int region_of_bits(uint32_t bits) { return bits == kUsa || bits == kEurope || bits == kJapan ? (int)bits : kNone; }

std::vector<std::string> mask_lines(const std::string& text) {
    std::vector<std::string> out;
    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        size_t a = pos, b = end;
        pos = end + 1;
        while (a < b && isspace((unsigned char)text[a])) a++;
        while (b > a && isspace((unsigned char)text[b - 1])) b--;  // CR LF
        if (a < b && text[a] != '#') out.push_back(text.substr(a, b - a));
    }
    return out;
}

int region_of_rom_type(const std::string& rom_type) {
    const std::string t = lower(rom_type);
    return t == "us" ? kUsa : t == "eu" ? kEurope : t == "jp" ? kJapan : kNone;
}

int region_of_packs(const std::vector<std::string>& names) {
    uint32_t found = 0;
    for (const std::string& n : names) {
        const std::string s = lower(n);  // Bootup_EUde.pack; not Bootup.pack or Bootup_Graphics.pack
        if (s.size() != 16 || s.compare(0, 7, "bootup_") != 0 || s.compare(11, 5, ".pack") != 0) continue;
        const std::string r = s.substr(7, 2);
        found |= r == "us" ? kUsa : r == "eu" ? kEurope : r == "jp" ? kJapan : kNone;
    }
    return region_of_bits(found);
}

Release decide(const std::vector<Source>& metadata, const std::vector<Source>& masks,
               const std::vector<std::string>& packs) {
    Release r;
    std::string title, title_from;
    for (const Source& s : metadata)
        if (title.empty() && !(title = title_of(s.text)).empty()) title_from = s.what;
    int rom = kNone;
    std::string rom_from;
    for (const Source& s : masks) {
        const std::vector<std::string> lines = mask_lines(s.text);
        if (lines.size() < 2) continue;
        rom = region_of_rom_type(lines[1]);
        rom_from = s.what;
        break;
    }
    if (const int region = region_of_title(title)) {
        r.region = region;
        r.title_id = title;
        r.from = "title in " + title_from;
        if (rom && rom != region) r.from += std::string("; the RomType says ") + name(rom) + " (" + rom_from + ")";
        return r;
    }
    int region = rom;
    std::string why = "the RomType in " + rom_from;
    if (!region) {
        region = region_of_packs(packs);
        why = "its Bootup packs";
    }
    for (size_t i = 0; !region && i < metadata.size(); i++) {
        const std::string bits = tag_text(metadata[i].text, "region");
        if (bits.empty()) continue;
        region = region_of_bits((uint32_t)strtoul(bits.c_str(), nullptr, 16));
        why = "the region in " + metadata[i].what;
        break;
    }
    if (!region) {
        region = kUsa;
        why = "nothing";
    }
    r.region = region;
    r.title_id = !title.empty() ? title : region == kEurope ? "00050000101C9500" : region == kJapan ? "00050000101C9300"
                                                                                                   : "00050000101C9400";
    r.from = (title.empty() ? std::string("no title ID") : "unknown title in " + title_from) + ", region from " + why;
    return r;
}

const Release& current() {
    static const Release release = [] {
        std::vector<Source> metadata;
        for (const char* rel : {"meta/meta.xml", "code/app.xml"})
            for (const std::string* root : {&config::game_dir, &config::base_dir})
                if (!root->empty()) metadata.push_back({*root + "/" + rel, read_small(fs::path(*root) / rel)});
        std::vector<Source> masks;  // the update has its copy in content/Local/System
        for (const char* rel : {"content/Local/System/RegionLangMask.txt", "content/System/RegionLangMask.txt"})
            for (const std::string* root : {&config::game_dir, &config::base_dir})
                if (!root->empty()) masks.push_back({*root + "/" + rel, read_small(fs::path(*root) / rel)});
        std::vector<std::string> packs = pack_names(config::game_dir), base = pack_names(config::base_dir);
        packs.insert(packs.end(), base.begin(), base.end());
        Release r = decide(metadata, masks, packs);
        LOG("[config] release: %s, title %s (%s)%s", name(r.region), r.title_id.c_str(), r.from.c_str(),
            r.region == kEurope ? "; the European release is untested so far" : "");
        return r;
    }();
    return release;
}

}  // namespace game_release

extern "C" const char* game_title_id(void) { return game_release::current().title_id.c_str(); }
