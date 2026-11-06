// The merged content of the player's content mods: see content_merge.h.
#include "content_merge.h"

#include "../exception_report.h"
#include "aamp.h"
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
// game already has is skipped, archives both have are merged file by file, anything else replaces.
bool apply_file(Archive& work, const Archive* vanilla, const std::string& name, const Blob& m, int mod, int depth,
                Ctx& ctx, const std::string& path) {
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
                    for (const auto& [inner, mn] : ma.files)
                        changed |= apply_file(*w.archive, &va, inner, mn.raw, mod, depth + 1, ctx, path + "/" + inner);
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
struct Job {
    std::string tree;
    fs::path vanilla;
    std::vector<Source> sources;
};

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

    // ---- loose files the game only has inside packs go into those packs
    std::optional<PackIndex> index;
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
        const int winner = job.sources.back().mod;
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
            bool changed = false;
            if (!src.inner.empty()) {
                changed = apply_file(*root.archive, &vanilla_archive, src.inner, m, src.mod, 1, ctx, where + "/" + src.inner);
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
                    changed |= apply_file(*root.archive, &vanilla_archive, inner, mn.raw, src.mod, 1, ctx, where + "/" + inner);
            }
            if (changed) {
                mods_in.insert(src.mod);
                contributed[src.mod].insert(src.host.string());
            }
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
