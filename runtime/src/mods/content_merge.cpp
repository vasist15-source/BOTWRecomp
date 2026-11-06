// The merged content of the player's content mods: see content_merge.h.
#include "content_merge.h"

#include "../exception_report.h"
#include "aamp.h"
#include "bcml_merge.h"
#include "botw_formats.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

namespace mods::merge {
namespace fs = std::filesystem;
using namespace mods::formats;

std::string tree_key(const std::string& tree) {
    std::string s = tree;
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

namespace {

constexpr int kMaxDepth = 4;  // archives in archives: pack > actor pack > ...
constexpr size_t kNotesPerMod = 12;
const std::string kSelfEntry = "System/Resource/ResourceSizeTable.product.rsizetable";

// ---------------------------------------------------------------- files
bool read_file(const fs::path& p, Bytes& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    out.resize(size_t(size));
    f.seekg(0);
    return size == 0 || bool(f.read(reinterpret_cast<char*>(out.data()), size));
}
size_t read_head(const fs::path& p, uint8_t* buf, size_t n) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return 0;
    f.read(reinterpret_cast<char*>(buf), std::streamsize(n));
    return size_t(f.gcount());
}
uint64_t file_unpacked_size(const fs::path& p) {
    uint8_t h[16];
    const size_t n = read_head(p, h, sizeof h);
    if (is_yaz0(h, n)) return yaz0_size(h, n);
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    return ec ? 0 : size;
}
// a SARC archive, or Yaz0 whose first literals spell one (no back reference can come before them)
bool archive_file(const fs::path& p) {
    uint8_t h[0x20];
    const size_t n = read_head(p, h, sizeof h);
    if (is_sarc(h, n)) return true;
    return is_yaz0(h, n) && n >= 21 && (h[16] & 0xF0) == 0xF0 && !memcmp(h + 17, "SARC", 4);
}
void write_file(const fs::path& p, const uint8_t* d, size_t n) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (f && n) f.write(reinterpret_cast<const char*>(d), std::streamsize(n));
    f.close();
    if (!f) exception_report::raise("Cannot write the merged file " + p.string());
}

// `rel` below `root`, any case (exact first: one stat on case-insensitive volumes)
fs::path resolve(const fs::path& root, const std::string& rel) {
    if (root.empty() || rel.empty()) return {};
    std::error_code ec;
    fs::path p = root / rel;
    if (fs::is_regular_file(p, ec)) return p;
    fs::path cur = root;
    size_t pos = 0;
    while (pos < rel.size()) {
        size_t slash = rel.find('/', pos);
        if (slash == std::string::npos) slash = rel.size();
        const std::string comp = rel.substr(pos, slash - pos);
        pos = slash + 1;
        if (comp.empty()) continue;
        fs::path next = cur / comp;
        if (!fs::exists(next, ec)) {
            next.clear();
            const std::string want = tree_key(comp);
            for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec))
                if (tree_key(it->path().filename().string()) == want) {
                    next = it->path();
                    break;
                }
            if (next.empty()) return {};
        }
        cur = next;
    }
    return fs::is_regular_file(cur, ec) ? cur : fs::path();
}

// tree paths: "content/<path>" or "aoc/<dlc folder>/<path>"
std::string aoc_prefix(const std::string& tree) {  // "Aoc/0010/" for an aoc tree, the size table's prefix
    if (tree.rfind("aoc/", 0) != 0) return {};
    const size_t slash = tree.find('/', 4);
    return slash == std::string::npos ? std::string() : "Aoc/" + tree.substr(4, slash - 4) + "/";
}
std::string inner_path(const std::string& tree) {  // the path below content/ or the DLC folder
    if (tree.rfind("content/", 0) == 0) return tree.substr(8);
    if (tree.rfind("aoc/", 0) == 0) {
        const size_t slash = tree.find('/', 4);
        return slash == std::string::npos ? tree.substr(4) : tree.substr(slash + 1);
    }
    return tree;
}
std::string shown(const std::string& tree) {  // as the Mods page shows it
    return tree.rfind("content/", 0) == 0 ? tree.substr(8) : tree;
}

// ---------------------------------------------------------------- bytes and archive trees
using Shared = std::shared_ptr<const Bytes>;
struct Blob {  // bytes shared with their owner (a file read once, or rebuilt data)
    Shared owner;
    size_t off = 0, len = 0;
    const uint8_t* data() const { return owner ? owner->data() + off : nullptr; }
};
Blob whole(Bytes b) {
    auto p = std::make_shared<const Bytes>(std::move(b));
    return {p, 0, p->size()};
}
bool same(const Blob& a, const Blob& b) {
    return a.len == b.len && (a.len == 0 || a.data() == b.data() || !memcmp(a.data(), b.data(), a.len));
}
bool unpack(const Blob& b, Blob& out) {
    if (!is_yaz0(b.data(), b.len)) {
        out = b;
        return true;
    }
    Bytes d;
    if (!yaz0_decode(b.data(), b.len, d)) return false;
    out = whole(std::move(d));
    return true;
}

struct Archive;
struct Node {  // a file in an archive (or a whole file at the top)
    Blob raw;  // as stored (Yaz0 or not), unless `archive` is set
    uint32_t alignment = 8;
    bool yaz0 = false;  // stored compressed: an expanded node is compressed again
    uint32_t yaz0_align = 0;
    std::unique_ptr<Archive> archive;  // expanded: changes go to its files
    int changed_by = -1;               // the mod (load order) that changed it last; -1: the game's
    uint64_t built_size = 0;           // after build(): unpacked size
};
struct Archive {
    uint32_t key = 0x65;
    std::map<std::string, Node> files;
};

// an unpacked archive's files, pointing into `data`; false if it isn't a big-endian SARC
bool open(const Blob& data, Archive& a, std::string* why = nullptr) {
    Sarc s;
    if (!sarc_parse(data.data(), data.len, s, why)) return false;
    if (!s.big_endian) {
        if (why) *why = "it is a little-endian (Switch) archive";
        return false;
    }
    a.key = s.key;
    a.files.clear();
    for (auto& e : s.entries) {
        if (e.name.empty()) {
            if (why) *why = "it has unnamed files";
            return false;
        }
        Node n;
        n.raw = {data.owner, data.off + e.offset, e.size};
        n.alignment = natural_alignment(e.offset);
        n.yaz0 = is_yaz0(n.raw.data(), n.raw.len);
        n.yaz0_align = yaz0_alignment(n.raw.data(), n.raw.len);
        a.files.emplace(std::move(e.name), std::move(n));
    }
    return true;
}
bool expand(Node& n) {
    if (n.archive) return true;
    Blob u;
    if (!unpack(n.raw, u) || !is_sarc(u.data(), u.len)) return false;
    auto a = std::make_unique<Archive>();
    if (!open(u, *a)) return false;
    n.archive = std::move(a);
    return true;
}
// the node as stored in its parent, expanded archives rebuilt (and compressed again if they were)
Blob build(Node& n) {
    if (!n.archive) {
        n.built_size = unpacked_size(n.raw.data(), n.raw.len);
        return n.raw;
    }
    std::vector<Blob> parts;
    std::vector<SarcInput> in;
    parts.reserve(n.archive->files.size());
    in.reserve(n.archive->files.size());
    for (auto& [name, child] : n.archive->files) {
        parts.push_back(build(child));
        in.push_back({name, parts.back().data(), parts.back().len, child.alignment});
    }
    Bytes sarc;
    std::string why;
    if (!sarc_write(std::move(in), n.archive->key, sarc, &why)) exception_report::raise("Cannot rebuild an archive: " + why);
    n.built_size = sarc.size();
    if (n.yaz0) return whole(yaz0_encode(sarc.data(), sarc.size(), n.yaz0_align));
    return whole(std::move(sarc));
}

// ---------------------------------------------------------------- merging
struct Candidate {  // a changed file: its size-table entry may have to grow
    std::string aoc;   // "Aoc/0010/" for DLC files
    std::string path;  // content-style path ("Actor/Pack/X.sbactorpack")
    int mod = -1;
    bool known = false;  // the game has this file
    uint64_t vanilla_size = 0, new_size = 0;
    // made here (a rebuilt archive, changed parameters), not a mod's file as it is: its entry must cover its
    // size even where the mod gives one (a .bnp's rstb.json sizes BCML's own rebuild of the file)
    bool built = false;
};
struct Ctx {
    // (lower mod, higher mod) -> files of the lower one the higher one replaced, and one of them
    std::map<std::pair<int, int>, std::pair<size_t, std::string>> conflicts;
    std::vector<std::pair<int, std::string>> notes;
    // files of this archive that BCML merge data rebuilds (the name in the archive -> mods with a log for it): a mod's
    // own copy of one is set aside (copies) and merged with the logs, entry by entry, instead of replacing it
    const std::map<std::string, std::set<int>>* rebuilt = nullptr;
    std::map<std::string, std::map<int, Blob>> copies;
    std::string host;  // the mod file being applied
    std::map<std::string, std::set<std::pair<int, std::string>>> set_aside;  // name -> (mod, file) whose copy of it was set aside
};

bool replace(Archive& work, const std::string& name, const Blob& raw, int mod, Ctx& ctx, const std::string& path) {
    auto it = work.files.find(name);
    if (it == work.files.end()) {
        const uint32_t h = sarc_hash(name, work.key);
        for (const auto& [other, node] : work.files)
            if (sarc_hash(other, work.key) == h) {
                ctx.notes.push_back({mod, path + " is not used: its name has the same hash as " + other});
                return false;
            }
        it = work.files.emplace(name, Node{}).first;
        it->second.alignment = data_alignment(raw.data(), raw.len);
    } else {
        Node& n = it->second;
        if (n.changed_by >= 0 && n.changed_by != mod) {
            auto& c = ctx.conflicts[{n.changed_by, mod}];
            if (!c.first++) c.second = path;
        }
        n.alignment = std::max(n.alignment, data_alignment(raw.data(), raw.len));
    }
    Node& n = it->second;
    n.raw = raw;
    if (raw.owner && raw.len * 4 < raw.owner->size())  // a small part of a mod's archive: keep only that part
        n.raw = whole(Bytes(raw.data(), raw.data() + raw.len));
    n.archive.reset();
    n.yaz0 = is_yaz0(raw.data(), raw.len);
    n.yaz0_align = yaz0_alignment(raw.data(), raw.len);
    n.changed_by = mod;
    return true;
}

// Puts a mod's version `m` of `name` into `work`, judged against the game's archive `vanilla`: what the
// game already has is skipped, archives both have are merged file by file, anything else replaces. `inner` is
// its path in the job's archive ("Actor/Pack/X.sbactorpack//Actor/ShopData/X.bshop").
bool apply_file(Archive& work, const Archive* vanilla, const std::string& name, const Blob& m, int mod, int depth,
                Ctx& ctx, const std::string& path, const std::string& inner) {
    if (ctx.rebuilt)
        if (auto it = ctx.rebuilt->find(inner); it != ctx.rebuilt->end()) {
            if (!it->second.count(mod)) ctx.copies[inner][mod] = m;  // its changes become its diff
            ctx.set_aside[inner].insert({mod, ctx.host});  // (a mod with a log for it: the log has its changes)
            return false;
        }
    const Node* v = nullptr;
    if (vanilla)
        if (auto it = vanilla->files.find(name); it != vanilla->files.end()) v = &it->second;
    if (v) {
        if (same(v->raw, m)) return false;
        Blob vu, mu;
        if (unpack(v->raw, vu) && unpack(m, mu)) {
            if (same(vu, mu)) return false;  // compressed differently, same data
            auto wit = work.files.find(name);
            Archive va, ma;
            if (depth < kMaxDepth && wit != work.files.end() && is_sarc(vu.data(), vu.len) && is_sarc(mu.data(), mu.len) &&
                open(vu, va) && open(mu, ma)) {
                Node& w = wit->second;
                const bool was_open = bool(w.archive);
                if (expand(w)) {
                    bool changed = false;
                    for (const auto& [child, mn] : ma.files)
                        changed |= apply_file(*w.archive, &va, child, mn.raw, mod, depth + 1, ctx, path + "/" + child, inner + "//" + child);
                    if (changed) w.changed_by = mod;
                    else if (!was_open) w.archive.reset();
                    return changed;
                }
            }
        }
    }
    return replace(work, name, m, mod, ctx, path);
}

// size-table candidates of a rebuilt archive, against the game's
void collect(const Archive& work, const Archive* vanilla, const std::string& aoc, int depth, std::vector<Candidate>& out) {
    for (const auto& [name, n] : work.files) {
        if (n.changed_by < 0) continue;
        const Node* v = nullptr;
        if (vanilla)
            if (auto it = vanilla->files.find(name); it != vanilla->files.end()) v = &it->second;
        out.push_back({aoc, name, n.changed_by, v != nullptr, v ? unpacked_size(v->raw.data(), v->raw.len) : 0, n.built_size, true});
        if (n.archive && depth < kMaxDepth) {
            Archive va;
            Blob vu;
            const bool have = v && unpack(v->raw, vu) && open(vu, va);
            collect(*n.archive, have ? &va : nullptr, aoc, depth + 1, out);
        }
    }
}
// whether a mod's archive has every file of the game's, also in the archives inside it that it changes
// (BCML's .bnp packs keep only the files they change: such a "partial" archive must be merged)
bool complete(const Archive& vanilla, const Archive& mod_archive, int depth) {
    for (const auto& [name, vn] : vanilla.files) {
        auto it = mod_archive.files.find(name);
        if (it == mod_archive.files.end()) return false;
        const Node& mn = it->second;
        if (depth >= kMaxDepth || same(vn.raw, mn.raw)) continue;
        Blob vu, mu;
        if (!unpack(vn.raw, vu) || !unpack(mn.raw, mu) || !is_sarc(vu.data(), vu.len) || !is_sarc(mu.data(), mu.len)) continue;
        Archive va, ma;
        if (open(vu, va) && open(mu, ma) && !complete(va, ma, depth + 1)) return false;
    }
    return true;
}
// size-table candidates of an archive one mod has, used as it is
void diff(const Archive& vanilla, const Archive& mod_archive, int mod, const std::string& aoc, int depth, std::vector<Candidate>& out) {
    for (const auto& [name, mn] : mod_archive.files) {
        auto it = vanilla.files.find(name);
        if (it == vanilla.files.end()) {
            out.push_back({aoc, name, mod, false, 0, unpacked_size(mn.raw.data(), mn.raw.len)});
            continue;
        }
        const Node& vn = it->second;
        if (same(vn.raw, mn.raw)) continue;
        Blob vu, mu;
        if (!unpack(vn.raw, vu) || !unpack(mn.raw, mu)) {
            out.push_back({aoc, name, mod, true, unpacked_size(vn.raw.data(), vn.raw.len), unpacked_size(mn.raw.data(), mn.raw.len)});
            continue;
        }
        if (same(vu, mu)) continue;
        out.push_back({aoc, name, mod, true, vu.len, mu.len});
        Archive va, ma;
        if (depth < kMaxDepth && is_sarc(vu.data(), vu.len) && is_sarc(mu.data(), mu.len) && open(vu, va) && open(mu, ma))
            diff(va, ma, mod, aoc, depth + 1, out);
    }
}

// ---------------------------------------------------------------- the game's packs
struct PackIndex {  // lower-case file name in a pack -> (pack tree, the name as the pack spells it)
    std::multimap<std::string, std::pair<std::string, std::string>> content, aoc;  // aoc: "<dlc folder>/<name>"
};
void index_pack(const fs::path& file, const std::string& tree, const std::string& prefix,
                std::multimap<std::string, std::pair<std::string, std::string>>& out) {
    uint8_t h[0x20];
    const size_t n = read_head(file, h, sizeof h);
    if (!is_sarc(h, n) || h[6] != 0xFE) return;
    const uint32_t data = be32(h + 0xC);
    if (data < 0x28 || data > (16u << 20)) return;
    std::error_code ec;
    const uint64_t size = fs::file_size(file, ec);
    if (ec) return;
    Bytes head(data);
    if (read_head(file, head.data(), data) != data) return;
    Sarc s;
    if (!sarc_parse(head.data(), head.size(), s, nullptr, size_t(size))) return;
    for (const auto& e : s.entries)
        if (!e.name.empty()) out.emplace(tree_key(prefix + e.name), std::pair{tree, e.name});
}
fs::path child_dir(const fs::path& dir, const std::string& name) {  // any case
    std::error_code ec;
    if (dir.empty()) return {};
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && tree_key(it->path().filename().string()) == tree_key(name)) return it->path();
    return {};
}
PackIndex index_packs(const Vanilla& v) {
    PackIndex idx;
    std::set<std::string> seen;
    for (const fs::path& content : {v.update_content, v.base_content}) {  // the update's packs first
        std::error_code ec;
        const fs::path dir = child_dir(content, "Pack");
        if (dir.empty()) continue;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (tree_key(it->path().extension().string()) != ".pack" || !seen.insert(tree_key(name)).second) continue;
            index_pack(it->path(), "content/Pack/" + name, "", idx.content);
        }
    }
    std::error_code ec;
    if (!v.aoc_content.empty())
        for (fs::directory_iterator d(v.aoc_content, ec), end; !ec && d != end; d.increment(ec)) {
            if (!d->is_directory(ec)) continue;
            const std::string folder = d->path().filename().string();
            const fs::path dir = child_dir(d->path(), "Pack");
            if (dir.empty()) continue;
            std::error_code ec2;
            for (fs::directory_iterator it(dir, ec2), e2; !ec2 && it != e2; it.increment(ec2))
                if (tree_key(it->path().extension().string()) == ".pack")
                    index_pack(it->path(), "aoc/" + folder + "/Pack/" + it->path().filename().string(), folder + "/", idx.aoc);
        }
    return idx;
}

// ---------------------------------------------------------------- BCML merge data (.bnp logs/)
struct Patch {  // a parameter change of deepmerge.aamp or aslist.aamp
    int mod = -1;
    std::string tree;                // the file it is in ("content/Pack/TitleBG.pack")
    std::vector<std::string> inner;  // its path inside, archive by archive ("Actor/Pack/X.sbactorpack", "Actor/...")
    aamp::List diff;
    bool aslist = false;
};
// The changes of a log: its FileTable names each file ("content/X.sbactorpack//Actor/Y.bphysics"); the list of
// a file's changes is named by the CRC-32 of that name.
bool read_patches(const fs::path& file, int mod, bool aslist, std::vector<Patch>& out, std::string& why) {
    Bytes b;
    aamp::File f;
    if (!read_file(file, b) || !aamp::parse(b.data(), b.size(), f, &why)) return false;
    std::map<uint32_t, std::string> names;
    if (const aamp::Object* table = aamp::object(f.root, aamp::hash("FileTable")))
        for (const auto& prm : table->params)
            if (aamp::is_string(prm.type)) names[aamp::hash(aamp::text(prm))] = aamp::text(prm);
    for (const auto& l : f.root.lists) {
        auto it = names.find(l.name);
        if (it == names.end()) continue;
        std::string path = it->second;
        std::replace(path.begin(), path.end(), '\\', '/');
        Patch p{mod, {}, {}, l, aslist};
        for (size_t at = 0;;) {
            const size_t sep = path.find("//", at);
            (p.tree.empty() ? p.tree : p.inner.emplace_back()) = path.substr(at, sep == std::string::npos ? std::string::npos : sep - at);
            if (sep == std::string::npos) break;
            at = sep + 2;
        }
        if (p.tree.rfind("content/", 0) != 0 && p.tree.rfind("aoc/", 0) != 0) {
            why = "it names files of another platform (" + p.tree + ")";
            return false;
        }
        out.push_back(std::move(p));
    }
    return true;
}
// rstb.json: {"Actor/Pack/X.bactorpack": 1234, ...}; 0 drops the entry (the game sizes such files itself)
bool read_size_log(const fs::path& file, std::map<std::string, int64_t>& out, std::string& why) {
    Bytes b;
    if (!read_file(file, b)) {
        why = "it can't be read";
        return false;
    }
    const std::string s(b.begin(), b.end());
    size_t i = 0;
    auto space = [&] {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
    };
    auto bad = [&](const char* what) {
        why = std::string(what) + " at byte " + std::to_string(i);
        return false;
    };
    space();
    if (i >= s.size() || s[i++] != '{') return bad("expected {");
    space();
    if (i < s.size() && s[i] == '}') return true;
    for (;;) {
        space();
        if (i >= s.size() || s[i++] != '"') return bad("expected a name");
        std::string name;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                const char e = s[++i];
                name += e == 'n' ? '\n' : e == 't' ? '\t' : e;
            } else {
                name += s[i];
            }
            i++;
        }
        if (i++ >= s.size()) return bad("unterminated name");
        space();
        if (i >= s.size() || s[i++] != ':') return bad("expected :");
        space();
        const size_t start = i;
        if (i < s.size() && s[i] == '-') i++;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        if (i == start || (i == start + 1 && s[start] == '-')) return bad("expected a whole number");
        out[name] = std::strtoll(s.c_str() + start, nullptr, 10);
        space();
        if (i < s.size() && s[i] == ',') {
            i++;
            continue;
        }
        if (i < s.size() && s[i] == '}') return true;
        return bad("expected , or }");
    }
}
// applies a parameter change to the file at p.inner[k..] below `work` (archives on the way are expanded)
bool apply_patch(Archive& work, const Patch& p, size_t k, Ctx& ctx, const std::string& where) {
    auto it = work.files.find(p.inner[k]);
    if (it == work.files.end()) {
        ctx.notes.push_back({p.mod, where + " is not in the game's files: its BCML change is skipped"});
        return false;
    }
    Node& n = it->second;
    if (k + 1 < p.inner.size()) {
        const bool was_open = bool(n.archive);
        if (!expand(n)) {
            ctx.notes.push_back({p.mod, where + " is not an archive: the BCML change inside it is skipped"});
            return false;
        }
        if (!apply_patch(*n.archive, p, k + 1, ctx, where + "/" + p.inner[k + 1])) {
            if (!was_open) n.archive.reset();
            return false;
        }
        n.changed_by = p.mod;
        return true;
    }
    Blob u;
    aamp::File f;
    std::string why;
    if (n.archive || !unpack(n.raw, u) || !aamp::parse(u.data(), u.len, f, &why)) {
        ctx.notes.push_back({p.mod, where + " can't be read as parameters" + (why.empty() ? "" : " (" + why + ")") + ": its BCML change is skipped"});
        return false;
    }
    if (p.aslist) aamp::merge_aslist(f.root, p.diff);
    else aamp::merge(f.root, p.diff);
    Bytes changed;
    if (!aamp::write(f, changed, &why)) {
        ctx.notes.push_back({p.mod, where + ": its BCML change can't be written (" + why + ")"});
        return false;
    }
    n.raw = n.yaz0 ? whole(yaz0_encode(changed.data(), changed.size(), n.yaz0_align)) : whole(std::move(changed));
    n.changed_by = p.mod;
    return true;
}

struct Source {  // something a mod changes in an archive the game has
    int mod;
    fs::path host;
    std::string inner;            // empty: the mod's own copy of the whole archive; else a loose file put in at this name
    const Patch* patch = nullptr;  // or a BCML parameter change inside it
    int order() const { return patch ? 2 : inner.empty() ? 0 : 1; }  // within a mod: its archive, loose files, changes
};
// A game data table of one file (bcml_merge.h's Kind: ActorInfo, a map unit, a shop, ...) that the mods' BCML merge
// data rebuilds: the game's copy with each mod's log or its own copy's changes, in load order, wherever the game reads
// the file (a map unit in Pack/AocMainField.pack and loose, a shop in its actor pack in Pack/TitleBG.pack, ...).
struct Target {
    bcml::Kind kind;
    std::string key;                       // its logs' name for it (map unit, shop or drop table file); "" for the one file of its kind
    std::map<int, const bcml::Diff*> logs;  // mod -> its log's changes
    struct Copy {  // a mod's own copy, set aside: its changes against the game's copy where it was become its diff
        int order = 0;  // within a mod: a copy inside an archive, then a loose one
        Blob copy, game;
        std::optional<bcml::Diff> diff;
        bool bad = false;
    };
    std::map<int, Copy> copies;
    std::set<std::pair<int, std::string>> hosts;              // (mod, its file) whose copy went in
    std::vector<std::pair<std::string, std::string>> places;  // (tree, path inside it; "": the loose file)
    std::set<int> noted;                                      // mods whose copy's changes were noted
    void add_copy(int mod, int order, const Blob& copy, const Blob& game) {
        if (logs.count(mod)) return;  // its log has its changes
        auto it = copies.find(mod);
        if (it != copies.end() && it->second.order > order) return;
        Copy& c = copies[mod];
        c = Copy{order, copy, game, std::nullopt, false};
    }
};
const char* kind_shown(bcml::Kind k) {  // what the summary calls a rebuilt file of a kind
    switch (k) {
    case bcml::Kind::ActorInfo: return "actors";
    case bcml::Kind::Map: return "map units";
    case bcml::Kind::Shop: return "shops";
    case bcml::Kind::Drop: return "drop tables";
    case bcml::Kind::Quests: return "quests";
    case bcml::Kind::Residents: return "resident actors";
    case bcml::Kind::AreaData: return "area data";
    case bcml::Kind::Effects: return "status effects";
    case bcml::Kind::MainStatic: return "main field static data";
    case bcml::Kind::DungeonStatic: return "shrine entrances";
    }
    return "tables";
}
// a file that BCML merge data for a game data table rebuilds, inside the archive of a job
struct Rebuild {
    enum Kind { Events, Flags, Save, Texts, Table } kind;
    std::string inner;         // its path in the archive ("Event/EventInfo.product.sbyml", "Actor/Pack/X.sbactorpack//Actor/ShopData/X.bshop")
    std::string language;      // Texts: the game's language ("USen")
    Target* target = nullptr;  // Table
};
std::vector<std::string> split_path(const std::string& path) {  // "A//B//C" -> A, B, C
    std::vector<std::string> out;
    for (size_t at = 0;;) {
        const size_t sep = path.find("//", at);
        out.push_back(path.substr(at, sep == std::string::npos ? std::string::npos : sep - at));
        if (sep == std::string::npos) return out;
        at = sep + 2;
    }
}
// A shop's or drop table's name by its actor pack, wherever a log says the pack is (BCML logs title actors in
// Pack/TitleBG.pack, or loose when a mod has them loose): "content/Actor/Pack/X.sbactorpack//Actor/ShopData/X.bshop"
std::string table_key(bcml::Kind kind, const std::string& name) {
    if (kind != bcml::Kind::Shop && kind != bcml::Kind::Drop) return name;
    const std::vector<std::string> parts = split_path(name);
    for (size_t k = 0; k + 1 < parts.size(); k++) {
        const std::string actor = k ? parts[k] : inner_path(parts[0]);
        const std::string lower = tree_key(actor);
        if (lower.size() <= 12 || lower.compare(lower.size() - 12, 12, ".sbactorpack")) continue;
        std::string key = (parts[0].rfind("aoc/", 0) == 0 ? parts[0].substr(0, parts[0].find('/', 4) + 1) : std::string("content/")) + actor;
        for (size_t j = k + 1; j < parts.size(); j++) key += "//" + parts[j];
        return key;
    }
    return name;
}
// The diffs of a target in load order: each mod's log, or its copy's changes (worked out once). `used`: their mods.
void target_diffs(Target& t, std::vector<const bcml::Diff*>& diffs, std::vector<int>& used, Ctx& ctx, const std::string& path) {
    std::set<int> order;
    for (const auto& [mod, d] : t.logs) order.insert(mod);
    for (const auto& [mod, c] : t.copies) order.insert(mod);
    for (int mod : order) {
        if (auto it = t.logs.find(mod); it != t.logs.end()) {
            diffs.push_back(it->second);
            used.push_back(mod);
            continue;
        }
        Target::Copy& c = t.copies.at(mod);
        if (!c.diff && !c.bad) {
            Blob cu, gu;
            std::string why;
            bcml::Diff d;
            if (!unpack(c.copy, cu) || !unpack(c.game, gu) ||
                !bcml::diff_of_copy(t.kind, Bytes(gu.data(), gu.data() + gu.len), Bytes(cu.data(), cu.data() + cu.len), d, why)) {
                c.bad = true;
                ctx.notes.push_back({mod, path + ": its copy can't be merged (" + (why.empty() ? std::string("it can't be read") : why) + ") and is not used"});
            } else {
                c.diff = std::move(d);
            }
        }
        if (c.bad) continue;
        const size_t n = bcml::entries(t.kind, *c.diff);
        if (!n) continue;
        if (t.noted.insert(mod).second)
            ctx.notes.push_back({mod, path + ": its changes (" + std::to_string(n) + (n == 1 ? " entry" : " entries") +
                                          ") are merged with other mods' BCML merge data"});
        diffs.push_back(&*c.diff);
        used.push_back(mod);
    }
}
// The game's copy `game` (as stored) merged with the target's diffs: `out` as stored (compressed again if the game's
// copy is). Returns the highest mod whose changes went in, -1 when nothing changes or it can't be merged (noted).
int merge_target(Target& t, const Blob& game, Blob& out, Ctx& ctx, const std::string& path) {
    std::vector<const bcml::Diff*> diffs;
    std::vector<int> used;
    target_diffs(t, diffs, used, ctx, path);
    if (diffs.empty()) return -1;
    Blob gu;
    if (!unpack(game, gu)) {
        ctx.notes.push_back({used.back(), path + ": the game's copy can't be read, so its BCML merge data is skipped"});
        return -1;
    }
    Bytes merged;
    size_t changed = 0;
    std::string why;
    if (!bcml::merge_file(t.kind, Bytes(gu.data(), gu.data() + gu.len), diffs, merged, changed, why)) {
        if (!why.empty()) ctx.notes.push_back({used.back(), path + ": BCML merge data can't be applied (" + why + ")"});
        return -1;
    }
    out = is_yaz0(game.data(), game.len) ? whole(yaz0_encode(merged.data(), merged.size(), yaz0_alignment(game.data(), game.len)))
                                         : whole(std::move(merged));
    return used.back();
}
// Rebuilds a target at `rb.inner` in `work` (archives on the way expanded) from the game's copy in `game`. Copies the
// job set aside there join the target's. Returns the highest mod whose changes went in (-1: none).
int rebuild_table(const Rebuild& rb, Archive& work, const Archive& game, const std::map<int, Blob>& set_aside, Ctx& ctx,
                  const std::string& where) {
    Target& t = *rb.target;
    const std::vector<std::string> parts = split_path(rb.inner);
    std::string path = where;
    for (const auto& p : parts) path += "/" + p;
    // the game's copy
    std::vector<std::unique_ptr<Archive>> hold;
    const Archive* ga = &game;
    const Node* vn = nullptr;
    for (size_t k = 0; k < parts.size() && ga; k++) {
        auto it = ga->files.find(parts[k]);
        if (it == ga->files.end()) break;
        if (k + 1 == parts.size()) {
            vn = &it->second;
            break;
        }
        Blob u;
        auto next = std::make_unique<Archive>();
        ga = unpack(it->second.raw, u) && open(u, *next) ? next.get() : nullptr;
        hold.push_back(std::move(next));
    }
    if (!vn) {
        for (const auto& [mod, d] : t.logs) ctx.notes.push_back({mod, path + " is not in the game's files: its BCML merge data is skipped"});
        return -1;
    }
    for (const auto& [mod, blob] : set_aside) t.add_copy(mod, 0, blob, vn->raw);
    Blob out;
    const int top = merge_target(t, vn->raw, out, ctx, path);
    if (top < 0) return -1;
    // the work archive's node (archives a mod changed on the way stay as they are)
    std::vector<Node*> chain;
    std::vector<bool> opened;
    Archive* wa = &work;
    Node* w = nullptr;
    for (size_t k = 0; k < parts.size(); k++) {
        auto it = wa->files.find(parts[k]);
        if (it == wa->files.end()) break;
        if (k + 1 == parts.size()) {
            w = &it->second;
            break;
        }
        const bool was_open = bool(it->second.archive);
        if (!expand(it->second)) break;
        chain.push_back(&it->second);
        opened.push_back(!was_open);
        wa = it->second.archive.get();
    }
    if (!w) {
        for (size_t k = chain.size(); k-- > 0;)
            if (opened[k] && chain[k]->changed_by < 0) chain[k]->archive.reset();
        ctx.notes.push_back({top, path + " is not where the game has it: its BCML merge data is skipped"});
        return -1;
    }
    w->raw = out;
    w->archive.reset();
    w->yaz0 = is_yaz0(out.data(), out.len);
    w->yaz0_align = yaz0_alignment(out.data(), out.len);
    w->alignment = std::max(w->alignment, vn->alignment);
    w->changed_by = top;
    for (Node* n : chain) n->changed_by = std::max(n->changed_by, top);
    return top;
}
struct Job {
    std::string tree;
    fs::path vanilla;
    std::vector<Source> sources;
    std::vector<Rebuild> rebuilds;  // applied after the sources, over the game's copies
};
// the mods' merge data for game data tables (bcml_merge.h), by mod
struct TableLogs {
    std::map<int, byml::Value> events, flags;
    std::map<int, bcml::SaveDiff> save;
    std::map<int, std::map<std::string, bcml::TextFiles>> texts;  // language -> files
    std::map<int, std::map<std::pair<bcml::Kind, std::string>, bcml::Diff>> tables;  // (kind, map unit/file or "") -> diff
    std::map<std::pair<bcml::Kind, std::string>, Target> targets;
    std::set<int> owners(const Rebuild& rb) const {
        std::set<int> out;
        auto take = [&](const auto& m) {
            for (const auto& e : m) out.insert(e.first);
        };
        switch (rb.kind) {
        case Rebuild::Events: take(events); break;
        case Rebuild::Flags: take(flags); break;
        case Rebuild::Save: take(save); break;
        case Rebuild::Texts: take(texts); break;
        case Rebuild::Table: take(rb.target->logs); break;
        }
        return out;
    }
};
// the game's languages: content/Pack/Bootup_<USen|EUde|...>.pack
std::vector<std::string> game_languages(const Vanilla& v) {
    std::set<std::string> out;
    for (const fs::path& content : {v.update_content, v.base_content}) {
        std::error_code ec;
        const fs::path dir = child_dir(content, "Pack");
        if (dir.empty()) continue;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string n = it->path().filename().string();
            if (n.size() == 16 && n.rfind("Bootup_", 0) == 0 && tree_key(n.substr(11)) == ".pack") out.insert(n.substr(7, 4));
        }
    }
    return {out.begin(), out.end()};
}
Bytes bytes_of(const Blob& b) { return Bytes(b.data(), b.data() + b.len); }
// an archive's files as stored (bcml::Files); false if it isn't one
bool files_of(const Blob& raw, bcml::Files& out, std::string* why = nullptr) {
    Blob u;
    Archive a;
    if (!unpack(raw, u) || !open(u, a, why)) return false;
    for (const auto& [name, n] : a.files) out[name] = bytes_of(n.raw);
    return true;
}
// Rebuilds `rb` in `work` from the game's copy in `game` and, in load order, each mod's log or its own copy's
// changes. Returns the highest mod whose changes went in (-1: none).
int rebuild(const Rebuild& rb, Archive& work, const Archive& game, const std::map<int, Blob>& copies, const TableLogs& tl, Ctx& ctx,
            const std::string& where) {
    const std::set<int> logged = tl.owners(rb);
    std::set<int> order = logged;
    for (const auto& [mod, blob] : copies) order.insert(mod);
    const std::string path = where + "/" + rb.inner;
    const auto vit = game.files.find(rb.inner);
    Blob gu;
    if (vit == game.files.end() || !unpack(vit->second.raw, gu)) {
        for (int mod : logged) ctx.notes.push_back({mod, path + " is not in the game's files: its BCML merge data is skipped"});
        return -1;
    }
    const Node& vn = vit->second;
    std::vector<int> used;
    std::string why;
    auto bad_copy = [&](int mod) { ctx.notes.push_back({mod, path + ": its copy can't be merged (" + why + ") and is not used"}); };
    auto derived_note = [&](int mod, const std::string& what) {
        ctx.notes.push_back({mod, path + ": its changes (" + what + ") are merged with other mods' BCML merge data"});
    };
    // the work archive's node, back to the game's copy (a mod's copy that replaced it is in `copies`)
    Node& w = work.files[rb.inner];
    w.raw = vn.raw;
    w.archive.reset();
    w.yaz0 = vn.yaz0;
    w.yaz0_align = vn.yaz0_align;
    w.alignment = std::max(w.alignment, vn.alignment);
    if (rb.kind == Rebuild::Events) {
        std::vector<std::unique_ptr<byml::Value>> derived;
        std::vector<const byml::Value*> diffs;
        const Bytes game_bytes = bytes_of(gu);
        for (int mod : order) {
            if (auto it = tl.events.find(mod); it != tl.events.end()) {
                diffs.push_back(&it->second);
                used.push_back(mod);
                continue;
            }
            Blob cu;
            auto d = std::make_unique<byml::Value>();
            if (!unpack(copies.at(mod), cu) || !bcml::events_of_copy(game_bytes, bytes_of(cu), *d, why)) {
                bad_copy(mod);
                continue;
            }
            if (d->hash.empty()) continue;
            derived_note(mod, std::to_string(d->hash.size()) + (d->hash.size() == 1 ? " event" : " events"));
            diffs.push_back(d.get());
            derived.push_back(std::move(d));
            used.push_back(mod);
        }
        Bytes out;
        size_t changed = 0;
        if (diffs.empty()) return -1;
        if (!bcml::merge_events(game_bytes, diffs, out, changed, why)) {
            ctx.notes.push_back({used.back(), path + ": BCML merge data can't be applied (" + why + ")"});
            return -1;
        }
        w.raw = vn.yaz0 ? whole(yaz0_encode(out.data(), out.size(), vn.yaz0_align)) : whole(std::move(out));
        w.changed_by = used.back();
        return used.back();
    }
    bcml::Files gf;
    if (!files_of(vn.raw, gf, &why)) {
        for (int mod : logged) ctx.notes.push_back({mod, path + " can't be read (" + why + "): its BCML merge data is skipped"});
        return -1;
    }
    std::map<int, bcml::Files> copy_files;
    for (const auto& [mod, blob] : copies)
        if (!logged.count(mod) && !files_of(blob, copy_files[mod], &why)) {
            copy_files.erase(mod);
            bad_copy(mod);
        }
    bcml::Files result;
    bool whole_set = false;  // result is the archive's whole new set of files
    size_t count = 0;
    bool ok = true;
    if (rb.kind == Rebuild::Flags) {
        std::vector<std::unique_ptr<byml::Value>> derived;
        std::vector<const byml::Value*> diffs;
        for (int mod : order) {
            if (auto it = tl.flags.find(mod); it != tl.flags.end()) {
                diffs.push_back(&it->second);
                used.push_back(mod);
            } else if (copy_files.count(mod)) {
                auto d = std::make_unique<byml::Value>();
                if (!bcml::flags_of_copy(gf, copy_files[mod], *d, why)) {
                    bad_copy(mod);
                    continue;
                }
                if (d->hash.empty()) continue;
                derived_note(mod, "game data flags");
                diffs.push_back(d.get());
                derived.push_back(std::move(d));
                used.push_back(mod);
            }
        }
        ok = diffs.empty() || bcml::merge_flags(gf, diffs, result, count, why);
    } else if (rb.kind == Rebuild::Save) {
        std::vector<std::unique_ptr<bcml::SaveDiff>> derived;
        std::vector<const bcml::SaveDiff*> diffs;
        for (int mod : order) {
            if (auto it = tl.save.find(mod); it != tl.save.end()) {
                diffs.push_back(&it->second);
                used.push_back(mod);
            } else if (copy_files.count(mod)) {
                auto d = std::make_unique<bcml::SaveDiff>();
                if (!bcml::save_of_copy(gf, copy_files[mod], *d, why)) {
                    bad_copy(mod);
                    continue;
                }
                if (d->add.empty() && d->del.empty()) continue;
                derived_note(mod, "save data flags");
                diffs.push_back(d.get());
                derived.push_back(std::move(d));
                used.push_back(mod);
            }
        }
        whole_set = true;
        why.clear();
        if (!diffs.empty() && !bcml::merge_save(gf, diffs, result, count, why)) {
            ok = why.empty();  // false with no reason: nothing changes
            result.clear();
            whole_set = false;
        }
    } else {
        std::vector<std::unique_ptr<bcml::TextFiles>> derived;
        std::vector<const bcml::TextFiles*> diffs;
        for (int mod : order) {
            if (auto it = tl.texts.find(mod); it != tl.texts.end()) {
                if (const bcml::TextFiles* t = bcml::texts_for(it->second, rb.language)) {
                    diffs.push_back(t);
                    used.push_back(mod);
                }
            } else if (copy_files.count(mod)) {
                auto d = std::make_unique<bcml::TextFiles>();
                if (!bcml::texts_of_copy(gf, copy_files[mod], *d, why)) {
                    bad_copy(mod);
                    continue;
                }
                if (d->empty()) continue;
                derived_note(mod, "texts");
                diffs.push_back(d.get());
                derived.push_back(std::move(d));
                used.push_back(mod);
            }
        }
        ok = diffs.empty() || bcml::merge_texts(gf, diffs, result, count, why);
    }
    if (!ok) {
        ctx.notes.push_back({used.empty() ? *logged.rbegin() : used.back(), path + ": BCML merge data can't be applied (" + why + ")"});
        return -1;
    }
    if (used.empty() || result.empty()) return -1;
    const int top = used.back();
    if (!expand(w)) {
        ctx.notes.push_back({top, path + " can't be opened: its BCML merge data is skipped"});
        return -1;
    }
    Archive& a = *w.archive;
    if (whole_set)
        for (auto it = a.files.begin(); it != a.files.end();) it = result.count(it->first) ? std::next(it) : a.files.erase(it);
    for (auto& [name, data] : result) {
        auto it = a.files.find(name);
        if (it != a.files.end() && it->second.raw.len == data.size() && !memcmp(it->second.raw.data(), data.data(), data.size())) continue;
        if (it == a.files.end()) {  // a new file: aligned as the game's files of its kind
            uint32_t align = 0;
            const std::string ext = name.substr(name.rfind('.') == std::string::npos ? name.size() : name.rfind('.'));
            for (const auto& [other, n] : a.files)
                if (other.size() >= ext.size() && !other.compare(other.size() - ext.size(), ext.size(), ext))
                    align = align ? std::min(align, n.alignment) : n.alignment;
            it = a.files.emplace(name, Node{}).first;
            it->second.alignment = align ? align : data_alignment(data.data(), data.size());
        }
        Node& n = it->second;
        n.raw = whole(std::move(data));
        n.archive.reset();
        n.yaz0 = is_yaz0(n.raw.data(), n.raw.len);
        n.yaz0_align = yaz0_alignment(n.raw.data(), n.raw.len);
        n.changed_by = top;
    }
    w.changed_by = top;
    return top;
}

}  // namespace

fs::path Vanilla::find(const std::string& tree) const {
    if (tree.rfind("content/", 0) == 0) {
        const std::string rel = tree.substr(8);
        fs::path p = resolve(update_content, rel);
        return p.empty() ? resolve(base_content, rel) : p;
    }
    if (tree.rfind("aoc/", 0) == 0) return resolve(aoc_content, tree.substr(4));
    return {};
}

Result build(const Vanilla& vanilla, const std::vector<Mod>& mods, const fs::path& out) {
    Result r;
    r.mods.resize(mods.size());
    auto note = [&](int mod, std::string text) {
        auto& n = r.mods[size_t(mod)].notes;
        if (n.size() < kNotesPerMod) n.push_back(std::move(text));
    };

    // ---- what each mod has
    struct Provided {
        int mod;
        std::string tree;
        fs::path host;
    };
    std::map<std::string, std::vector<Provided>> by_key;  // in load order
    for (int i = 0; i < int(mods.size()); i++) {
        for (const auto& [root, prefix] : {std::pair<fs::path, const char*>{mods[size_t(i)].content, "content"},
                                           std::pair<fs::path, const char*>{mods[size_t(i)].aoc, "aoc"}}) {
            if (root.empty()) continue;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
                 it.increment(ec)) {
                const std::string name = it->path().filename().string();
                if (name.empty() || name[0] == '.') {  // .DS_Store, ._ files, hidden folders
                    if (it->is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (it->is_symlink(ec) || !it->is_regular_file(ec) || name == "Thumbs.db" || name == "desktop.ini") continue;
                const std::string tree = std::string(prefix) + "/" + it->path().lexically_relative(root).generic_string();
                auto& list = by_key[tree_key(tree)];
                if (!list.empty() && list.back().mod == i) list.back() = {i, tree, it->path()};  // same path twice (case)
                else list.push_back({i, tree, it->path()});
                r.mods[size_t(i)].files++;
            }
        }
    }
    std::vector<Provided> tables;  // the mods' own size tables: merged, never used as they are
    if (auto it = by_key.find(tree_key(kSizeTable)); it != by_key.end()) {
        tables = std::move(it->second);
        by_key.erase(it);
    }

    // ---- BCML merge data: parameter changes go into the archives they name, sizes into the size table
    std::map<std::string, Job> jobs;
    std::vector<Patch> patches;
    std::map<int, std::map<std::string, int64_t>> logged_sizes;  // mod -> rstb.json
    for (int i = 0; i < int(mods.size()); i++) {
        const fs::path& logs = mods[size_t(i)].logs;
        if (logs.empty()) continue;
        std::error_code ec;
        for (const auto& [name, aslist] : {std::pair<const char*, bool>{"deepmerge.aamp", false}, {"aslist.aamp", true}}) {
            if (!fs::exists(logs / name, ec)) continue;
            std::string why;
            if (!read_patches(logs / name, i, aslist, patches, why)) note(i, std::string("Its ") + name + " can't be used: " + why);
        }
        if (fs::exists(logs / "rstb.json", ec)) {
            std::string why;
            if (!read_size_log(logs / "rstb.json", logged_sizes[i], why)) note(i, "Its rstb.json can't be used: " + why);
        }
    }
    std::map<int, size_t> patched;  // mod -> parameter files its BCML data changed
    std::vector<const Patch*> loose_patches;  // changes of loose parameter files
    for (const auto& p : patches) {
        if (p.inner.empty()) {
            loose_patches.push_back(&p);
            continue;
        }
        Job& j = jobs[tree_key(p.tree)];
        if (j.tree.empty()) {
            j.tree = p.tree;
            j.vanilla = vanilla.find(p.tree);
        }
        j.sources.push_back({p.mod, {}, {}, &p});
    }

    // ---- BCML merge data for game data tables (events, flags, save data, texts): rebuilt over the game's copies
    TableLogs tables_logged;
    std::optional<PackIndex> index;  // the game's packs' files (made when needed)
    std::map<std::string, std::vector<Target*>> loose_targets;  // tree key of a loose file BCML merge data rebuilds -> its targets
    std::map<bcml::Kind, std::set<std::string>> rebuilt_tables;  // kind -> the files of it rebuilt (for the summary)
    std::vector<std::string> table_notes;  // for the summary
    {
        std::map<int, std::vector<std::string>> what;  // mod -> what its logs change
        for (int i = 0; i < int(mods.size()); i++) {
            const fs::path& logs = mods[size_t(i)].logs;
            if (logs.empty()) continue;
            auto text_of = [&](const char* name, std::string& out) {
                std::error_code ec;
                if (!fs::is_regular_file(logs / name, ec)) return false;
                Bytes b;
                if (!read_file(logs / name, b)) {
                    note(i, std::string("Its ") + name + " can't be read");
                    return false;
                }
                out.assign(b.begin(), b.end());
                return true;
            };
            auto plural = [](size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
            std::string text, why;
            if (text_of("eventinfo.yml", text)) {
                byml::Value v;
                if (!bcml::read_events(text, v, why)) note(i, "Its eventinfo.yml can't be used: " + why);
                else if (!v.hash.empty()) {
                    what[i].push_back(plural(v.hash.size(), "event", "events"));
                    tables_logged.events[i] = std::move(v);
                }
            }
            if (text_of("gamedata.yml", text)) {
                byml::Value v;
                if (!bcml::read_flags(text, v, why)) note(i, "Its gamedata.yml can't be used: " + why);
                else {
                    size_t n = 0;
                    for (const auto& [type, d] : v.hash) {
                        if (const byml::Value* add = d.find("add")) n += add->hash.size();
                        if (const byml::Value* del = d.find("del")) n += del->array.size();
                    }
                    if (n) {
                        what[i].push_back(plural(n, "game data flag", "game data flags"));
                        tables_logged.flags[i] = std::move(v);
                    }
                }
            }
            if (text_of("savedata.yml", text)) {
                bcml::SaveDiff d;
                if (!bcml::read_save(text, d, why)) note(i, "Its savedata.yml can't be used: " + why);
                else if (!d.add.empty() || !d.del.empty()) {
                    what[i].push_back(plural(d.add.size() + d.del.size(), "save data flag", "save data flags"));
                    tables_logged.save[i] = std::move(d);
                }
            }
            if (text_of("texts.json", text)) {
                std::map<std::string, bcml::TextFiles> t;
                if (!bcml::read_texts(text, t, why)) note(i, "Its texts.json can't be used: " + why);
                else if (!t.empty()) {
                    size_t files = 0, texts = 0;  // of its largest language
                    for (const auto& [lang, f] : t) {
                        size_t n = 0;
                        for (const auto& [file, entries] : f) n += entries.size();
                        if (n > texts) texts = n, files = f.size();
                    }
                    what[i].push_back(plural(texts, "text", "texts") + " in " + plural(files, "file", "files"));
                    tables_logged.texts[i] = std::move(t);
                }
            }
            // tables of one file each (bcml_tables.cpp)
            struct One {
                const char* log;
                bcml::Kind kind;
                const char *one, *many;
            };
            for (const One& o : {One{"actorinfo.yml", bcml::Kind::ActorInfo, "actor", "actors"}, One{"quests.yml", bcml::Kind::Quests, "quest", "quests"},
                                 One{"residents.yml", bcml::Kind::Residents, "resident actor", "resident actors"},
                                 One{"areadata.yml", bcml::Kind::AreaData, "area", "areas"},
                                 One{"effects.yml", bcml::Kind::Effects, "status effect", "status effects"},
                                 One{"mainstatic.yml", bcml::Kind::MainStatic, "main field static entry", "main field static entries"},
                                 One{"dstatic.yml", bcml::Kind::DungeonStatic, "shrine entrance", "shrine entrances"}}) {
                if (!text_of(o.log, text)) continue;
                bcml::Diff d;
                if (!bcml::read_log(o.kind, text, d, why)) {
                    note(i, std::string("Its ") + o.log + " can't be used: " + why);
                    continue;
                }
                const size_t n = bcml::entries(o.kind, d);
                if (!n) continue;
                what[i].push_back(plural(n, o.one, o.many));
                tables_logged.tables[i][{o.kind, ""}] = std::move(d);
            }
            // logs of a file each for several files: map units, shops, drop tables
            auto several = [&](bcml::Kind kind, std::map<std::string, bcml::Diff>& files, const char* one, const char* many, const char* file,
                               const char* files_name) {
                size_t n = 0, count = 0;
                for (auto& [name, d] : files) {
                    const size_t e = bcml::entries(kind, d);
                    if (!e) continue;
                    n += e;
                    count++;
                    auto [it, fresh] = tables_logged.tables[i].try_emplace({kind, table_key(kind, name)}, std::move(d));
                    if (fresh) continue;
                    bcml::Diff& same = it->second;  // the same file under two names
                    aamp::merge(same.shop, d.shop);
                    for (auto& table : d.drops) {
                        auto at = std::find_if(same.drops.begin(), same.drops.end(), [&](const auto& x) { return x.first == table.first; });
                        if (at != same.drops.end()) at->second = std::move(table.second);
                        else same.drops.push_back(std::move(table));
                    }
                }
                if (n) what[i].push_back(plural(n, one, many) + " in " + plural(count, file, files_name));
            };
            if (text_of("map.yml", text)) {
                std::map<std::string, bcml::Diff> units;
                if (!bcml::read_maps(text, units, why)) note(i, "Its map.yml can't be used: " + why);
                else several(bcml::Kind::Map, units, "map object", "map objects", "map unit", "map units");
            }
            if (std::error_code ec; fs::is_regular_file(logs / "shop.aamp", ec)) {
                Bytes b;
                std::map<std::string, bcml::Diff> shops;
                if (!read_file(logs / "shop.aamp", b)) note(i, "Its shop.aamp can't be read");
                else if (!bcml::read_shops(b, shops, why)) note(i, "Its shop.aamp can't be used: " + why);
                else several(bcml::Kind::Shop, shops, "shop item", "shop items", "shop", "shops");
            }
            if (text_of("drops.json", text)) {
                std::map<std::string, bcml::Diff> drops;
                if (!bcml::read_drops(text, drops, why)) note(i, "Its drops.json can't be used: " + why);
                else several(bcml::Kind::Drop, drops, "drop table", "drop tables", "file", "files");
            }
        }
        for (const auto& [mod, parts] : what) {
            std::string s;
            for (const auto& p : parts) s += (s.empty() ? "" : ", ") + p;
            note(mod, "BCML merge data: " + s);
        }
        const std::string boot = "content/Pack/Bootup.pack";
        auto add = [&](const std::string& pack, Rebuild rb) {
            Job& j = jobs[tree_key(pack)];
            if (j.tree.empty()) {
                j.tree = pack;
                j.vanilla = vanilla.find(pack);
            }
            j.rebuilds.push_back(std::move(rb));
        };
        if (!tables_logged.events.empty()) add(boot, {Rebuild::Events, "Event/EventInfo.product.sbyml", {}});
        if (!tables_logged.flags.empty()) add(boot, {Rebuild::Flags, "GameData/gamedata.ssarc", {}});
        if (!tables_logged.save.empty()) add(boot, {Rebuild::Save, "GameData/savedataformat.ssarc", {}});
        if (!tables_logged.texts.empty())
            for (const std::string& lang : game_languages(vanilla))  // every language of the game, as BCML's "all languages"
                add("content/Pack/Bootup_" + lang + ".pack", {Rebuild::Texts, "Message/Msg_" + lang + ".product.ssarc", lang});

        // tables of one file each: rebuilt where the game reads them. With the DLC, map units are the DLC's (its static
        // ones in Pack/AocMainField.pack, which the game reads before the loose copies), and so are the main field's
        // and the shrines' static data; shops and drop tables are in their actor pack wherever the game has it (title
        // actors in Pack/TitleBG.pack)
        const bool dlc_maps = !vanilla.find("aoc/0010/Pack/AocMainField.pack").empty();
        auto places_of = [&](bcml::Kind kind, const std::string& key) {
            std::vector<std::pair<std::string, std::string>> p;
            auto in_pack = [&](const std::string& pack, const std::string& inner) {
                if (!vanilla.find(pack).empty()) p.push_back({pack, inner});
            };
            auto loose = [&](const std::string& tree) {
                if (!vanilla.find(tree).empty()) p.push_back({tree, ""});
            };
            auto dlc_or_boot = [&](const std::string& rel) {
                if (!vanilla.find("aoc/0010/" + rel).empty()) loose("aoc/0010/" + rel);
                else in_pack(boot, rel);
            };
            switch (kind) {
            case bcml::Kind::ActorInfo: loose("content/Actor/ActorInfo.product.sbyml"); break;
            case bcml::Kind::Quests: in_pack("content/Pack/TitleBG.pack", "Quest/QuestProduct.sbquestpack"); break;
            case bcml::Kind::Residents: in_pack(boot, "Actor/ResidentActors.byml"); break;
            case bcml::Kind::AreaData: in_pack(boot, "Ecosystem/AreaData.sbyml"); break;
            case bcml::Kind::Effects: in_pack(boot, "Ecosystem/StatusEffectList.sbyml"); break;
            case bcml::Kind::MainStatic: dlc_or_boot("Map/MainField/Static.smubin"); break;
            case bcml::Kind::DungeonStatic: dlc_or_boot("Map/CDungeon/Static.smubin"); break;
            case bcml::Kind::Map: {  // "A-1_Static"
                const size_t us = key.find('_');
                if (us == std::string::npos || us == 0 || key.find_first_of("/\\.") != std::string::npos) break;
                const std::string type = key.substr(us + 1), rel = "Map/MainField/" + key.substr(0, us) + "/" + key + ".smubin";
                if (type == "Static" && dlc_maps) {
                    in_pack("aoc/0010/Pack/AocMainField.pack", rel);
                    loose("aoc/0010/" + rel);
                } else if (type == "Static") {
                    in_pack("content/Pack/TitleBG.pack", rel);
                } else if (type == "Dynamic") {
                    loose((dlc_maps ? "aoc/0010/" : "content/") + rel);
                }
                break;
            }
            case bcml::Kind::Shop: case bcml::Kind::Drop: {  // "content/Actor/Pack/X.sbactorpack//Actor/ShopData/X.bshop"
                const std::vector<std::string> parts = split_path(key);
                if (parts.size() < 2 || (parts[0].rfind("content/", 0) != 0 && parts[0].rfind("aoc/", 0) != 0)) break;
                size_t actor_at = parts.size();
                for (size_t k = 0; k + 1 < parts.size() && actor_at == parts.size(); k++) {
                    const std::string name = tree_key(k ? parts[k] : inner_path(parts[0]));
                    if (name.size() > 12 && !name.compare(name.size() - 12, 12, ".sbactorpack")) actor_at = k;
                }
                std::string rest;
                for (size_t k = actor_at == parts.size() ? 1 : actor_at + 1; k < parts.size(); k++) rest += (rest.empty() ? "" : "//") + parts[k];
                if (actor_at == parts.size()) {  // not in an actor pack: where the log says
                    in_pack(parts[0], rest);
                    break;
                }
                const std::string actor = actor_at ? parts[actor_at] : inner_path(parts[0]);
                const std::string prefix = parts[0].rfind("aoc/", 0) == 0 ? parts[0].substr(0, parts[0].find('/', 4) + 1) : "content/";
                in_pack(prefix + actor, rest);
                if (!index) index = index_packs(vanilla);
                const bool aoc = prefix != "content/";
                auto& packs = aoc ? index->aoc : index->content;
                for (auto [it, end] = packs.equal_range(tree_key(aoc ? prefix.substr(4) + actor : actor)); it != end; ++it)
                    p.push_back({it->second.first, it->second.second + "//" + rest});
                break;
            }
            }
            std::sort(p.begin(), p.end());
            p.erase(std::unique(p.begin(), p.end()), p.end());
            return p;
        };
        for (const auto& [mod, logged] : tables_logged.tables)
            for (const auto& [id, d] : logged) {
                auto [it, fresh] = tables_logged.targets.try_emplace(id);
                Target& t = it->second;
                if (fresh) {
                    t.kind = id.first;
                    t.key = id.second;
                    t.places = places_of(id.first, id.second);
                }
                t.logs[mod] = &d;
            }
        for (auto& [id, t] : tables_logged.targets) {
            if (t.places.empty()) {
                const std::string file = t.key.empty() ? std::string(kind_shown(t.kind)) : t.key;
                for (const auto& [mod, d] : t.logs) note(mod, file + " is not in the game's files: its BCML merge data is skipped");
            }
            for (const auto& [tree, inner] : t.places) {
                if (inner.empty()) loose_targets[tree_key(tree)].push_back(&t);
                else add(tree, {Rebuild::Table, inner, {}, &t});
            }
        }
    }

    // ---- loose files the game only has inside packs go into those packs
    std::map<std::pair<int, std::string>, size_t> injected;  // (mod, pack) -> files
    std::set<std::string> injected_keys;                       // mod files that go into packs
    for (const auto& [key, list] : by_key) {
        const Provided& top = list.back();
        if (!vanilla.find(top.tree).empty()) continue;
        if (!index) index = index_packs(vanilla);
        const bool dlc = top.tree.rfind("aoc/", 0) == 0;
        const std::string lookup = dlc ? tree_key(top.tree.substr(4)) : tree_key(inner_path(top.tree));
        auto& map = dlc ? index->aoc : index->content;
        for (auto [it, end] = map.equal_range(lookup); it != end; ++it) {
            const auto& [pack_tree, inner] = it->second;
            Job& j = jobs[tree_key(pack_tree)];
            if (j.tree.empty()) {
                j.tree = pack_tree;
                j.vanilla = vanilla.find(pack_tree);
            }
            j.sources.push_back({top.mod, top.host, inner});
            injected[{top.mod, shown(pack_tree)}]++;
            injected_keys.insert(key);
        }
    }
    for (const auto& [where, count] : injected)
        note(where.first, std::to_string(count) + (count == 1 ? " file goes" : " files go") + " into " + where.second +
                              ", where the game reads " + (count == 1 ? "it" : "them"));

    // ---- each path: one mod's file, or an archive merged from several
    std::vector<Candidate> candidates;
    std::map<std::pair<int, int>, std::pair<size_t, std::string>> overridden;  // (lower, higher) -> count, example
    for (auto& [key, list] : by_key) {
        const Provided& top = list.back();
        const fs::path vfile = vanilla.find(top.tree);
        if (auto lt = loose_targets.find(key); lt != loose_targets.end()) {
            // a loose file BCML merge data rebuilds (after the archives): the mods' copies are merged into it
            Bytes gb;
            Blob game_blob;
            if (read_file(vfile, gb)) game_blob = whole(std::move(gb));
            for (const auto& p : list) {
                Bytes mb;
                if (!read_file(p.host, mb)) {
                    note(p.mod, shown(p.tree) + " can't be read");
                    continue;
                }
                const Blob m = whole(std::move(mb));
                for (Target* t : lt->second) {
                    t->add_copy(p.mod, 1, m, game_blob);
                    t->hosts.insert({p.mod, p.host.string()});
                }
            }
            continue;
        }
        const bool pending = jobs.count(key) > 0;
        const bool game_archive = !vfile.empty() && archive_file(vfile);
        bool merge = game_archive && (list.size() >= 2 || pending);
        for (const auto& p : list) merge = merge && archive_file(p.host);
        std::vector<Candidate> inside;  // what one mod's archive changes, when it is used as it is
        if (!merge && game_archive && list.size() == 1 && archive_file(top.host)) {
            // one mod's copy of an archive the game has: used as it is when it has all of the game's files,
            // else (BCML's .bnp packs keep only the files they change) merged into the game's copy
            Bytes vb, mb;
            Blob vu, mu;
            Archive va, ma;
            std::string why;
            if (read_file(vfile, vb) && read_file(top.host, mb)) {
                const Blob vblob = whole(std::move(vb)), mblob = whole(std::move(mb));
                if (unpack(vblob, vu) && open(vu, va)) {
                    if (!unpack(mblob, mu) || !open(mu, ma, &why)) {
                        note(top.mod, shown(top.tree) + " is not used: it can't be read" + (why.empty() ? "" : " (" + why + ")"));
                        r.mods[size_t(top.mod)].overridden++;
                        continue;
                    }
                    if (!complete(va, ma, 1)) {
                        merge = true;
                        note(top.mod, shown(top.tree) + " has only the files it changes (as BCML packs them): they go into the game's copy");
                    } else {
                        diff(va, ma, top.mod, aoc_prefix(top.tree), 1, inside);
                    }
                }
            }
        }
        if (merge) {
            Job& j = jobs[key];
            j.tree = top.tree;
            j.vanilla = vfile;
            for (const auto& p : list) j.sources.push_back({p.mod, p.host, {}});
            continue;
        }
        if (pending) {  // a mod's non-archive copy wins over files put into the game's archive
            note(top.mod, shown(top.tree) + " is not an archive: files other mods put into it are not used");
            jobs.erase(key);
        }
        r.files[key] = top.host.string();  // (also for a file that goes into packs: the game may read it loose too)
        if (!injected_keys.count(key)) r.mods[size_t(top.mod)].replaced++;
        for (size_t k = 0; k + 1 < list.size(); k++) {
            r.mods[size_t(list[k].mod)].overridden++;
            auto& o = overridden[{list[k].mod, top.mod}];
            if (!o.first++) o.second = shown(top.tree);
        }
        candidates.push_back({aoc_prefix(top.tree), inner_path(top.tree), top.mod, !vfile.empty(),
                              vfile.empty() ? 0 : file_unpacked_size(vfile), file_unpacked_size(top.host)});
        candidates.insert(candidates.end(), inside.begin(), inside.end());
    }

    // ---- merged archives
    std::map<int, std::set<std::string>> contributed;  // mod -> its files that changed a merged archive
    std::set<std::string> rebuilt_loose;  // rebuilt files' loose paths: a mod's loose copy is not read instead
    for (auto& [key, job] : jobs) {
        std::stable_sort(job.sources.begin(), job.sources.end(), [](const Source& a, const Source& b) {
            return a.mod != b.mod ? a.mod < b.mod : a.order() < b.order();
        });
        Bytes vbytes;
        Archive vanilla_archive;
        Node root;
        std::string why;
        bool ok = !job.vanilla.empty() && read_file(job.vanilla, vbytes);
        if (ok) {
            root.raw = whole(std::move(vbytes));
            root.yaz0 = is_yaz0(root.raw.data(), root.raw.len);
            root.yaz0_align = yaz0_alignment(root.raw.data(), root.raw.len);
            Blob vu;
            ok = unpack(root.raw, vu) && open(vu, vanilla_archive, &why) && expand(root);
        }
        std::map<std::string, std::set<int>> rebuilt;  // files BCML merge data rebuilds here -> the mods with a log for them
        for (const auto& rb : job.rebuilds) rebuilt[rb.inner] = tables_logged.owners(rb);
        int winner = job.sources.empty() ? -1 : job.sources.back().mod;
        for (const auto& [inner, owners] : rebuilt)
            if (!owners.empty()) winner = std::max(winner, *owners.rbegin());
        if (!ok) {  // the game's archive can't be read: the copy of the mod highest in the list, if any
            for (auto it = job.sources.rbegin(); it != job.sources.rend(); ++it)
                if (it->inner.empty() && !it->patch) {
                    r.files[key] = it->host.string();
                    r.mods[size_t(it->mod)].replaced++;
                    break;
                }
            note(winner, shown(job.tree) + ": the game's copy can't be read" + (why.empty() ? "" : " (" + why + ")") + ", so nothing is merged");
            continue;
        }
        r.vanilla_used.push_back(job.vanilla);
        Ctx ctx;
        if (!rebuilt.empty()) ctx.rebuilt = &rebuilt;
        std::set<int> mods_in;
        for (const auto& src : job.sources) {
            if (src.patch) {
                if (apply_patch(*root.archive, *src.patch, 0, ctx, shown(job.tree) + "/" + src.patch->inner[0])) {
                    mods_in.insert(src.mod);
                    patched[src.mod]++;
                }
                continue;
            }
            Bytes mb;
            if (!read_file(src.host, mb)) {
                note(src.mod, shown(src.inner.empty() ? job.tree : src.host.filename().string()) + " can't be read");
                continue;
            }
            const Blob m = whole(std::move(mb));
            const std::string where = shown(job.tree);
            ctx.host = src.host.string();
            bool changed = false;
            if (!src.inner.empty()) {
                changed = apply_file(*root.archive, &vanilla_archive, src.inner, m, src.mod, 1, ctx, where + "/" + src.inner, src.inner);
            } else {
                Blob mu;
                Archive ma;
                std::string bad;
                if (!unpack(m, mu) || !open(mu, ma, &bad)) {
                    note(src.mod, where + " can't be merged" + (bad.empty() ? "" : " (" + bad + ")") + " and is not used");
                    r.mods[size_t(src.mod)].overridden++;
                    continue;
                }
                for (const auto& [inner, mn] : ma.files)
                    changed |= apply_file(*root.archive, &vanilla_archive, inner, mn.raw, src.mod, 1, ctx, where + "/" + inner, inner);
            }
            if (changed) {
                mods_in.insert(src.mod);
                contributed[src.mod].insert(src.host.string());
            }
        }
        // game data tables: the game's copy with every mod's log (or its own copy's changes), over the mods' copies
        for (const auto& rb : job.rebuilds) {
            const int top = rb.kind == Rebuild::Table
                                ? rebuild_table(rb, *root.archive, vanilla_archive, ctx.copies[rb.inner], ctx, shown(job.tree))
                                : rebuild(rb, *root.archive, vanilla_archive, ctx.copies[rb.inner], tables_logged, ctx, shown(job.tree));
            if (top < 0) continue;
            for (const auto& [mod, host] : ctx.set_aside[rb.inner]) contributed[mod].insert(host);  // read through the rebuilt file
            if (job.tree.rfind("content/", 0) == 0 && rb.inner.find("//") == std::string::npos)
                rebuilt_loose.insert(tree_key("content/" + rb.inner));
            for (int mod : tables_logged.owners(rb)) mods_in.insert(mod);
            for (const auto& [mod, blob] : ctx.copies[rb.inner]) mods_in.insert(mod);
            if (rb.kind == Rebuild::Table) {
                for (const auto& [mod, host] : rb.target->hosts) contributed[mod].insert(host);
                for (const auto& [mod, c] : rb.target->copies) mods_in.insert(mod);
                rebuilt_tables[rb.target->kind].insert(rb.target->key);
                continue;
            }
            table_notes.push_back(rb.kind == Rebuild::Events ? "events" : rb.kind == Rebuild::Flags ? "game data flags"
                                  : rb.kind == Rebuild::Save ? "save data" : "texts " + rb.language);
        }
        const Blob built = build(root);
        const fs::path file = out / job.tree;
        write_file(file, built.data(), built.len);
        r.files[key] = file.string();
        r.merged_archives++;
        collect(*root.archive, &vanilla_archive, aoc_prefix(job.tree), 1, candidates);
        // the archive itself (root.raw is still the game's copy)
        candidates.push_back({aoc_prefix(job.tree), inner_path(job.tree), winner, true, unpacked_size(root.raw.data(), root.raw.len),
                              root.built_size, true});
        for (const auto& [mod, text] : ctx.notes) note(mod, text);
        for (const auto& [pair, c] : ctx.conflicts) {  // files in the archive both changed: the higher one's
            auto& o = overridden[pair];
            o.first += c.first;
            if (o.second.empty()) o.second = c.second;
        }
        if (mods_in.size() >= 2) {
            std::string names;  // as listed: the one that wins first
            for (auto it = mods_in.rbegin(); it != mods_in.rend(); ++it) names += (names.empty() ? "" : ", ") + mods[size_t(*it)].name;
            for (int mod : mods_in) note(mod, shown(job.tree) + ": merged (" + names + ")");
        }
    }
    for (const auto& key : rebuilt_loose) r.files.erase(key);
    // ---- loose files BCML merge data rebuilds (ActorInfo, map units, the DLC's static data), with every copy the mods have
    for (auto& [id, t] : tables_logged.targets)
        for (const auto& [tree, inner] : t.places) {
            if (!inner.empty()) continue;
            const fs::path game_file = vanilla.find(tree);
            Bytes gb;
            if (!read_file(game_file, gb)) {
                for (const auto& [mod, d] : t.logs) note(mod, shown(tree) + " can't be read: its BCML merge data is skipped");
                continue;
            }
            const Blob game_blob = whole(std::move(gb));
            Ctx ctx;
            Blob merged;
            const int top = merge_target(t, game_blob, merged, ctx, shown(tree));
            for (const auto& [mod, text] : ctx.notes) note(mod, text);
            if (top < 0) continue;
            const fs::path file = out / tree;
            write_file(file, merged.data(), merged.len);
            r.files[tree_key(tree)] = file.string();
            r.vanilla_used.push_back(game_file);
            candidates.push_back({aoc_prefix(tree), inner_path(tree), top, true, unpacked_size(game_blob.data(), game_blob.len),
                                  unpacked_size(merged.data(), merged.len), true});
            for (const auto& [mod, host] : t.hosts) contributed[mod].insert(host);
            rebuilt_tables[t.kind].insert(t.key);
        }
    // loose parameter files a BCML change names
    for (const Patch* p : loose_patches) {
        const std::string key = tree_key(p->tree);
        const fs::path game_file = vanilla.find(p->tree);
        const auto mapped = r.files.find(key);
        const fs::path base = mapped != r.files.end() ? fs::path(mapped->second) : game_file;
        Bytes b, changed;
        aamp::File f;
        Blob raw, u;
        std::string why;
        if (base.empty() || !read_file(base, b)) {
            note(p->mod, shown(p->tree) + " is not in the game's files: its BCML change is skipped");
            continue;
        }
        raw = whole(std::move(b));
        if (!unpack(raw, u) || !aamp::parse(u.data(), u.len, f, &why)) {
            note(p->mod, shown(p->tree) + " can't be read as parameters" + (why.empty() ? "" : " (" + why + ")"));
            continue;
        }
        if (p->aslist) aamp::merge_aslist(f.root, p->diff);
        else aamp::merge(f.root, p->diff);
        if (!aamp::write(f, changed, &why)) {
            note(p->mod, shown(p->tree) + ": its BCML change can't be written (" + why + ")");
            continue;
        }
        const size_t size = changed.size();
        const Bytes stored = is_yaz0(raw.data(), raw.len) ? yaz0_encode(changed.data(), changed.size(), yaz0_alignment(raw.data(), raw.len))
                                                          : std::move(changed);
        const fs::path file = out / p->tree;
        write_file(file, stored.data(), stored.size());
        r.files[key] = file.string();
        candidates.push_back({aoc_prefix(p->tree), inner_path(p->tree), p->mod, !game_file.empty(),
                              game_file.empty() ? 0 : file_unpacked_size(game_file), size, true});
        patched[p->mod]++;
    }
    for (const auto& [mod, count] : patched)
        note(mod, "BCML merge data: " + std::to_string(count) + (count == 1 ? " parameter file" : " parameter files") + " changed");
    if (!table_notes.empty() || !rebuilt_tables.empty()) {  // "Rebuilt from BCML merge data: events, texts USen, USes, actors, 3 map units"
        std::string s, langs;
        for (const auto& t : table_notes) {
            if (t.rfind("texts ", 0) == 0) langs += (langs.empty() ? "" : ", ") + t.substr(6);
            else s += (s.empty() ? "" : ", ") + t;
        }
        if (!langs.empty()) s += (s.empty() ? "texts " : ", texts ") + langs;
        for (const auto& [kind, keys] : rebuilt_tables) {
            std::string what = kind_shown(kind);
            if (kind == bcml::Kind::Map || kind == bcml::Kind::Shop || kind == bcml::Kind::Drop)  // "2 map units", "1 shop"
                what = std::to_string(keys.size()) + " " + (keys.size() == 1 ? what.substr(0, what.size() - 1) : what);
            s += (s.empty() ? "" : ", ") + what;
        }
        r.log.push_back("Rebuilt from BCML merge data: " + s);
    }

    for (const auto& [mod, files] : contributed) r.mods[size_t(mod)].merged += files.size();
    for (const auto& [pair, o] : overridden)
        note(pair.first, std::to_string(o.first) + (o.first == 1 ? " file is" : " files are") + " used from " +
                             mods[size_t(pair.second)].name + " instead (higher in the list), e.g. " + o.second);

    // ---- the resource size table
    const fs::path table_file = vanilla.find(kSizeTable);
    if (!table_file.empty() && (!candidates.empty() || !tables.empty() || !logged_sizes.empty())) {
        Bytes raw, body;
        Rstb game;
        std::string why;
        if (!read_file(table_file, raw) || !yaz0_decode(raw.data(), raw.size(), body) || !rstb_parse(body.data(), body.size(), game, &why)) {
            r.log.push_back("The game's resource size table can't be read" + (why.empty() ? "" : " (" + why + ")") + ": sizes are left as they are");
        } else {
            r.vanilla_used.push_back(table_file);
            Rstb t = game;
            std::map<int, Rstb> own;
            for (const auto& p : tables) {
                Bytes mr, mbody;
                Rstb mt;
                std::string bad;
                const bool read = read_file(p.host, mr);
                const bool unpacked = read && (is_yaz0(mr.data(), mr.size()) ? yaz0_decode(mr.data(), mr.size(), mbody) : (mbody = mr, true));
                if (!unpacked || !rstb_parse(mbody.data(), mbody.size(), mt, &bad)) {
                    note(p.mod, "Its resource size table can't be read" + (bad.empty() ? "" : " (" + bad + ")") + "; sizes are worked out instead");
                    continue;
                }
                for (const auto& [crc, v] : mt.crc)
                    if (auto it = game.crc.find(crc); it == game.crc.end() || it->second != v) t.crc[crc] = std::max(t.crc[crc], v);
                for (const auto& [name, v] : mt.names)
                    if (auto it = game.names.find(name); it == game.names.end() || it->second != v) t.names[name] = std::max(t.names[name], v);
                r.mods[size_t(p.mod)].merged++;
                own[p.mod] = std::move(mt);
            }
            size_t grown = 0, removed = 0, logged = 0;
            // the sizes of .bnp mods (rstb.json), in load order: 0 drops the entry
            std::map<int, std::set<std::string>> logged_names;
            for (const auto& [mod, sizes] : logged_sizes) {
                for (const auto& [name, v] : sizes) {
                    logged_names[mod].insert(name);
                    logged++;
                    uint32_t now = 0;
                    const bool have = rstb_get(t, name, &now);
                    if (v <= 0) {
                        if (have) {
                            rstb_erase(t, name);
                            removed++;
                        }
                        continue;
                    }
                    const uint32_t size = uint32_t(std::min<int64_t>(v, 0xFFFFFFFF));
                    if (!have || size > now) rstb_set(t, name, size);
                }
            }
            for (const auto& c : candidates) {
                std::vector<std::string> names;
                const std::string base = resource_name(c.path);
                if (!c.aoc.empty()) names.push_back(c.aoc + base);
                names.push_back(base);
                std::string pick;
                uint32_t entry = 0;
                for (const auto& n : names)
                    if (rstb_get(game, n, &entry)) {
                        pick = n;
                        break;
                    }
                if (pick.empty()) continue;  // the game sizes it without the table (or it is new)
                uint32_t now = 0;
                const bool listed = rstb_get(t, pick, &now);
                if (auto l = logged_names.find(c.mod); l != logged_names.end() && l->second.count(pick)) {
                    if (!c.built || !listed) continue;  // its own size (or none: the game works it out)
                } else if (auto o = own.find(c.mod); o != own.end()) {
                    uint32_t mine;
                    if (!rstb_get(o->second, pick, &mine)) {
                        rstb_erase(t, pick);  // the mod's table leaves it out, so the game works the size out
                        removed++;
                        continue;
                    }
                    if (!c.built) continue;  // the mod's own value, merged above
                }
                if (!c.known) continue;
                const uint32_t want = grown_entry(entry, c.vanilla_size, c.new_size);
                if (want > now) {
                    rstb_set(t, pick, want);
                    grown++;
                }
            }
            if (!(t == game)) {
                uint32_t self = 0;  // the table's own entry: the memory the table itself takes
                if (rstb_get(game, kSelfEntry, &self)) {
                    const uint64_t overhead = self > body.size() ? self - body.size() : 0;
                    const uint64_t size = 12 + uint64_t(t.crc.size()) * 8 + uint64_t(t.names.size()) * 132;
                    uint32_t now = 0;
                    rstb_get(t, kSelfEntry, &now);
                    if (size + overhead > now) rstb_set(t, kSelfEntry, uint32_t(size + overhead));
                }
                const Bytes table = rstb_write(t);
                const Bytes packed = yaz0_encode(table.data(), table.size(), yaz0_alignment(raw.data(), raw.size()));
                const fs::path file = out / kSizeTable;
                write_file(file, packed.data(), packed.size());
                r.files[tree_key(kSizeTable)] = file.string();
                for (const auto& [crc, v] : t.crc)
                    if (auto it = game.crc.find(crc); it == game.crc.end() || it->second != v) r.size_entries++;
                for (const auto& [name, v] : t.names)
                    if (auto it = game.names.find(name); it == game.names.end() || it->second != v) r.size_entries++;
                r.size_entries += removed;
                r.log.push_back("Resource size table: " + std::to_string(r.size_entries) + " entries differ from the game's (" +
                                std::to_string(own.size()) + " tables and " + std::to_string(logged) + " sizes from mods, " +
                                std::to_string(grown) + " grown, " + std::to_string(removed) + " left out)");
            }
        }
    }

    size_t files = 0;
    for (const auto& m : r.mods) files += m.files;
    const size_t options = size_t(std::count_if(mods.begin(), mods.end(), [](const Mod& m) { return m.option; }));
    const size_t whole = mods.size() - options;
    r.log.insert(r.log.begin(), std::to_string(whole) + (whole == 1 ? " mod" : " mods") +
                                    (options ? " with " + std::to_string(options) + (options == 1 ? " option" : " options") : std::string()) +
                                    ", " + std::to_string(files) + " files: " + std::to_string(r.files.size()) + " paths replaced, " +
                                    std::to_string(r.merged_archives) + " archives merged");
    return r;
}

}  // namespace mods::merge
