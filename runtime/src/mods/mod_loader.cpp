// Content mods (Settings > Mods): see mod_loader.h.
#include "mod_loader.h"

#include "../exception_report.h"
#include "../game_release.h"
#include "../platform/host.h"
#include "../platform/process.h"
#include "../runtime.h"
#include "cemu_pack.h"
#include "content_merge.h"
#include "mod_hash.h"
#include "mod_json.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace mods::loader {
namespace fs = std::filesystem;

namespace {

constexpr int kCacheFormat = 4;     // bump when merged output changes for the same inputs (3: compact AAMP, sizes of rebuilt files; 4: BCML events, flags, save data, texts)
constexpr size_t kSaveBackups = 10;  // newest kept in "Save backups"

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
bool hidden(const std::string& name) {
    return name.empty() || name[0] == '.' || name == "__MACOSX" || name == "Thumbs.db" || name == "desktop.ini";
}
bool title_folder(const std::string& name) {  // 16 hex digits
    return name.size() == 16 && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isxdigit(c); });
}
bool botw_title(const std::string& id) {  // Japan, USA, Europe
    const std::string t = lower(id);
    return t == "00050000101c9300" || t == "00050000101c9400" || t == "00050000101c9500";
}
std::string read_text(const fs::path& p, size_t limit) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) > limit) return {};
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
void write_text(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
        if (!f) exception_report::raise("Cannot write " + tmp.string());
    }
    fs::rename(tmp, p, ec);
    if (ec) exception_report::raise("Cannot write " + p.string());
}

// rules.txt: [Definition] name / path, description and titleIds (Cemu's format)
void read_rules(const fs::path& rules, Layout& l) {
    std::istringstream in(read_text(rules, 1 << 20));
    std::string line, section, name, path, description, titles;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#' || line[start] == ';') continue;
        if (line[start] == '[') {
            section = lower(line.substr(start + 1, line.find(']', start) - start - 1));
            continue;
        }
        if (section != "definition") continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(line.substr(start, eq - start)), value = line.substr(eq + 1);
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        if (key == "name") name = value;
        else if (key == "path") path = value;
        else if (key == "description") description = value;
        else if (key == "titleids") titles = value;
    }
    if (!name.empty()) l.name = name;
    else if (!path.empty()) l.name = path.substr(path.find_last_of('/') + 1);
    std::replace(description.begin(), description.end(), '|', ' ');  // Cemu's line breaks
    l.description = description;
    if (!titles.empty() && !mods::cemu::targets_title(titles, "00050000101C9300") &&
        !mods::cemu::targets_title(titles, "00050000101C9400") && !mods::cemu::targets_title(titles, "00050000101C9500"))
        l.problem = "This graphic pack is for another game (its titleIds don't name Breath of the Wild)";
}
// A BCML mod (.bnp) keeps some changes only as merge data in logs/. The merge applies deepmerge.aamp and
// aslist.aamp (actor parameters, animation lists), rstb.json (sizes), eventinfo.yml, gamedata.yml, savedata.yml
// and texts.json (events, game data and save data flags, texts: bcml_merge.h); packs.json only lists its packs.
// Anything else (ActorInfo, maps, shops, drops, quests, ...) is not applied yet, and without it the game can crash
// (The Linkle Mod unpacked before this support: support bones and physics of its new models, 2026-11-06).
const char* const kBcmlLogs[] = {"packs.json", "rstb.json", "deepmerge.aamp", "aslist.aamp",
                                 "eventinfo.yml", "gamedata.yml", "savedata.yml", "texts.json"};
bool bcml_logs(const fs::path& dir, std::string& unsupported) {  // whether dir/logs has any; which can't be applied
    std::error_code ec;
    std::vector<std::string> other;
    bool any = false;
    for (fs::directory_iterator it(dir / "logs", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (hidden(n) || !it->is_regular_file(ec)) continue;
        any = true;
        if (std::none_of(std::begin(kBcmlLogs), std::end(kBcmlLogs), [&](const char* k) { return lower(n) == k; })) other.push_back(n);
    }
    std::sort(other.begin(), other.end());
    unsupported.clear();
    for (const auto& n : other) unsupported += (unsupported.empty() ? "" : ", ") + n;
    return any;
}
// info.json of a BCML mod: its name, description and options ({"multi": [{name, desc, folder, default}],
// "single": [{name, desc, required, options: [{name, desc, folder}]}]}, as BCML's dev tools write them). An
// option is its folder, as BCML selects them: a folder named twice is the first one. Options whose folder
// is missing are left out, and so are options whose logs can't be applied (named in the warning).
void read_bcml_info(const fs::path& dir, Layout& l) {
    const std::string info = read_text(dir / "info.json", 1 << 20);
    if (info.empty()) return;
    try {
        const json::Value v = json::parse(info);
        if (!v.get("name").string().empty()) l.name = v.get("name").string();
        l.description = v.get("desc").string();
        std::vector<std::pair<const json::Value*, int>> choices;  // option, group (-1: multi)
        std::vector<Layout::Group> groups;
        for (const auto& o : v.get("options").get("multi").array) choices.push_back({&o, -1});
        for (const auto& group : v.get("options").get("single").array) {
            groups.push_back({group.get("name").string(), group.get("desc").string(), group.get("required").boolean});
            for (const auto& o : group.get("options").array) choices.push_back({&o, int(groups.size()) - 1});
        }
        std::string skipped;
        std::vector<int> group_index(groups.size(), -1);  // in l.groups: the groups left with options
        for (const auto& [o, group] : choices) {
            const std::string folder = o->get("folder").string();
            std::error_code ec;
            if (folder.empty() || folder == "." || folder == ".." || folder.find_first_of("/\\") != std::string::npos ||
                !fs::is_directory(dir / "options" / folder, ec))
                continue;
            if (std::any_of(l.options.begin(), l.options.end(), [&](const Layout::Option& x) { return x.key == folder; })) continue;
            const std::string name = o->get("name").string(folder);
            std::string unsupported;
            bcml_logs(dir / "options" / folder, unsupported);
            if (!unsupported.empty()) {
                skipped += (skipped.empty() ? "" : ", ") + name;
                continue;
            }
            int g = -1;
            if (group >= 0) {
                if (group_index[size_t(group)] < 0) {
                    group_index[size_t(group)] = int(l.groups.size());
                    l.groups.push_back(groups[size_t(group)]);
                }
                g = group_index[size_t(group)];
            }
            l.options.push_back({folder, name, o->get("desc").string(), dir / "options" / folder, g, o->get("default").boolean});
        }
        if (!skipped.empty()) l.warning = "Options with merge data that can't be applied yet, so they can't be chosen: " + skipped + ".";
    } catch (const std::exception&) {
    }
}
bool cemu_files(const fs::path& dir, bool code) {  // Cemu code patches (code) or replacement shaders next to rules.txt
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = lower(it->path().filename().string());
        if (code && ((n.rfind("patch_", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".asm") || n == "patches.txt")) return true;
        if (!code && n.size() > 7 && (n.substr(n.size() - 7) == "_ps.txt" || n.substr(n.size() - 7) == "_vs.txt")) return true;
    }
    return false;
}
bool merged(const Layout& l) {  // it takes part in the merge at startup (a code mod alone doesn't)
    return !l.content.empty() || !l.aoc.empty() || !l.logs.empty() || !l.options.empty();
}

using Choices = std::map<std::string, bool>;  // a mod's options chosen in Settings > Mods: key -> on
struct Saved {
    std::vector<std::pair<std::string, bool>> order;  // top wins
    std::map<std::string, Choices> options;  // by mod; options not in it take BCML's default
    std::string last_set;  // the set of mods of the last start ("none", or its fingerprint)
};
// the mods a start uses, top first, each with the keys of the options it uses
using Inputs = std::vector<std::pair<std::string, std::vector<std::string>>>;
struct State {
    bool started = false, off = false;
    fs::path dir, settings, cache;
    Saved saved;
    std::map<std::string, Layout> layouts;
    Inputs active;  // this run
    std::map<std::string, merge::ModReport> reports;  // by input id (option_id for an option)
    std::string summary;
};
std::mutex g_mutex;
State g;
std::unordered_map<std::string, std::string> g_files;  // tree key -> host file; frozen once g_ready
std::atomic<bool> g_ready{false};

std::string option_id(const std::string& mod, const std::string& key) { return mod + "/" + key; }  // its merge input

Saved load_saved(const fs::path& p) {
    Saved s;
    const std::string text = read_text(p, 1 << 20);
    if (text.empty()) return s;
    try {
        const json::Value v = json::parse(text);
        for (const auto& m : v.get("mods").array) {
            const std::string id = m.get("id").string();
            if (id.empty()) continue;
            s.order.push_back({id, m.get("enabled").type != json::Value::Bool || m.get("enabled").boolean});
            for (const auto& [key, on] : m.get("options").object)
                if (on.type == json::Value::Bool) s.options[id][key] = on.boolean;
        }
        s.last_set = v.get("last_set").string();
    } catch (const std::exception& e) {
        LOG("[mods] %s: %s (choices reset)", p.string().c_str(), e.what());
    }
    return s;
}
void save_locked() {
    json::Value v;
    v["format_version"] = 1;
    json::Value& entries = v["mods"];
    entries.type = json::Value::Array;
    for (const auto& [id, on] : g.saved.order) {
        json::Value m;
        m["id"] = id;
        m["enabled"] = on;
        if (auto o = g.saved.options.find(id); o != g.saved.options.end() && !o->second.empty())
            for (const auto& [key, chosen] : o->second) m["options"][key] = chosen;
        entries.array.push_back(m);
    }
    v["last_set"] = g.saved.last_set;
    try {
        write_text(g.settings, json::dump(v) + "\n");
    } catch (const std::exception& e) {
        LOG("[mods] %s", e.what());
    }
}
fs::path cache_root();
fs::path unpack_root() {  // unpacked .bnp files, next to the merge caches
    if (const char* e = std::getenv("BOTW_MODS_CACHE")) return fs::path(e).parent_path() / "Mods unpacked";
    return cache_root().parent_path() / "Mods unpacked";
}
std::string unpack_name(const fs::path& bnp) {  // changes with the file
    std::error_code ec;
    std::string stem = bnp.stem().string();
    for (char& c : stem)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != ' ' && c != '.') c = '_';
    return stem + "-" + std::to_string(fs::file_size(bnp, ec)) + "-" +
           std::to_string((long long)fs::last_write_time(bnp, ec).time_since_epoch().count());
}
bool unpack_bnp(const fs::path& bnp, const fs::path& dest, std::string& why) {
    std::error_code ec;
    const fs::path tmp = dest.string() + ".partial";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
#ifdef __APPLE__
    const char* tar = "/usr/bin/tar";  // bsdtar: reads 7z (.bnp), refuses absolute and .. paths
#else
    const char* tar = "bsdtar";
#endif
    const auto t0 = std::chrono::steady_clock::now();
    const host::ProcessResult r = host::run_process({tar, "-xf", bnp.string(), "-C", tmp.string()});
    if (r.code != 0 || !r.error.empty()) {
        std::string text = r.error.empty() ? r.output : r.error;
        if (const size_t nl = text.find('\n'); nl != std::string::npos) text.resize(nl);
        why = "tar failed" + (text.empty() ? std::string() : ": " + text);
        fs::remove_all(tmp, ec);
        return false;
    }
    for (fs::recursive_directory_iterator it(tmp, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_symlink(ec)) {
            why = "it contains links";
            fs::remove_all(tmp, ec);
            return false;
        }
    fs::remove_all(dest, ec);
    fs::rename(tmp, dest, ec);
    if (ec) {
        why = "can't move it into place (" + ec.message() + ")";
        fs::remove_all(tmp, ec);
        return false;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOG("[mods] unpacked %s in %.0f ms (%s)", bnp.filename().string().c_str(), ms, dest.string().c_str());
    return true;
}
// a .bnp in the Mods folder: unpacked at startup (unpack), else only listed
Layout bnp_layout(const fs::path& bnp, const std::string& title, bool unpack, std::vector<std::string>& unpacked) {
    std::error_code ec;
    const std::string name = unpack_name(bnp);
    const fs::path dest = unpack_root() / name;
    unpacked.push_back(name);
    Layout l;
    l.kind = "BCML mod (.bnp)";
    l.name = bnp.stem().string();
    if (!fs::is_directory(dest, ec)) {
        if (!unpack) {
            l.warning = "It is unpacked when the game starts next";
            return l;
        }
        std::string why;
        if (!unpack_bnp(bnp, dest, why)) {
            l.problem = "This .bnp can't be unpacked: " + why;
            return l;
        }
    }
    Layout d = detect(dest, title);
    if (d.problem.empty() && d.content.empty() && d.aoc.empty()) d.problem = "There is no content/ or aoc/ folder in this .bnp";
    d.kind = "BCML mod (.bnp)";
    return d;
}
void scan_locked(bool unpack = false) {
    g.layouts.clear();
    std::error_code ec;
    const std::string title = game_title_id();
    std::vector<std::string> unpacked;
    for (fs::directory_iterator it(g.dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (hidden(name)) continue;
        if (!it->is_directory(ec) && lower(it->path().extension().string()) == ".bnp")
            g.layouts[name] = bnp_layout(it->path(), title, unpack, unpacked);
        else
            g.layouts[name] = detect(it->path(), title);
    }
    if (unpack)  // unpacked .bnp files that are gone or changed
        for (fs::directory_iterator it(unpack_root(), ec), end; !ec && it != end; it.increment(ec))
            if (std::find(unpacked.begin(), unpacked.end(), it->path().filename().string()) == unpacked.end()) fs::remove_all(it->path(), ec);
    std::vector<std::pair<std::string, bool>> order, fresh;
    for (const auto& entry : g.saved.order)
        if (g.layouts.count(entry.first) && std::none_of(order.begin(), order.end(), [&](const auto& o) { return o.first == entry.first; }))
            order.push_back(entry);
    for (const auto& [id, layout] : g.layouts)
        if (std::none_of(order.begin(), order.end(), [&](const auto& o) { return o.first == id; })) fresh.push_back({id, true});
    order.insert(order.begin(), fresh.begin(), fresh.end());  // new mods come first: they win
    if (order != g.saved.order) {
        g.saved.order = std::move(order);
        for (auto it = g.saved.options.begin(); it != g.saved.options.end();)  // a mod that is gone takes its options along
            it = std::any_of(g.saved.order.begin(), g.saved.order.end(), [&](const auto& o) { return o.first == it->first; })
                     ? std::next(it)
                     : g.saved.options.erase(it);
        save_locked();
    }
}
const Choices& choices_locked(const std::string& id) {
    static const Choices none;
    auto it = g.saved.options.find(id);
    return it == g.saved.options.end() ? none : it->second;
}
Inputs wanted_locked() {  // the mods and options the saved choices use, top first
    Inputs out;
    for (const auto& [id, on] : g.saved.order) {
        auto it = g.layouts.find(id);
        if (!on || it == g.layouts.end() || !it->second.problem.empty() || !merged(it->second)) continue;
        const Layout& l = it->second;
        const std::vector<bool> use = chosen(l, choices_locked(id));
        std::vector<std::string> keys;
        for (size_t k = 0; k < l.options.size(); k++)
            if (use[k]) keys.push_back(l.options[k].key);
        out.push_back({id, std::move(keys)});
    }
    return out;
}
const std::vector<std::string>* active_locked(const std::string& id) {  // this run's options of the mod, null: not active
    for (const auto& a : g.active)
        if (a.first == id) return &a.second;
    return nullptr;
}

fs::path content_of(const std::string& dir) {
    if (dir.empty()) return {};
    std::error_code ec;
    const fs::path p = fs::path(dir) / "content";
    return fs::is_directory(p, ec) ? p : fs::path();
}
fs::path cache_root() {
    if (const char* e = std::getenv("BOTW_MODS_CACHE")) return e;
    if (std::getenv("BOTW_MODS_DIR")) return g.dir.parent_path() / "Mods cache";
    if (host::portable()) return fs::path(host::portable_user_dir()) / "cache" / "Mods";
#ifdef __APPLE__
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / "Library" / "Caches" / "BotW" / "Mods";
#else
    return fs::path(host::config_dir()) / "cache" / "Mods";
#endif
}

// the inputs of a merge: the game's folders and every file of the mods (path, size, time), in order. A chosen
// option is an input of its own (option_id), so each choice of options is a set of its own.
std::string fingerprint(const merge::Vanilla& v, const std::vector<merge::Mod>& inputs) {
    std::ostringstream s;
    s << "botw content mods " << kCacheFormat << '\n' << v.update_content.string() << '\n' << v.base_content.string() << '\n'
      << v.aoc_content.string() << '\n';
    for (const auto& m : inputs) {
        s << "mod " << m.id << '\n';
        for (const auto& [root, tag] : {std::pair<fs::path, const char*>{m.content, "content"}, std::pair<fs::path, const char*>{m.aoc, "aoc"},
                                        std::pair<fs::path, const char*>{m.logs, "logs"}}) {
            if (root.empty()) continue;
            std::vector<std::string> lines;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
                 it.increment(ec)) {
                const std::string name = it->path().filename().string();
                if (name.empty() || name[0] == '.') {
                    if (it->is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(ec)) continue;
                const auto size = it->file_size(ec);
                const long long time = (long long)it->last_write_time(ec).time_since_epoch().count();
                lines.push_back(it->path().lexically_relative(root).generic_string() + '\t' + std::to_string(size) + '\t' +
                                std::to_string(time));
            }
            std::sort(lines.begin(), lines.end());
            for (const auto& l : lines) s << tag << '/' << l << '\n';
        }
    }
    return hash::sha256_text(s.str()).substr(0, 32);
}
std::string stamp(const fs::path& p) {  // size and time of a game file the cache depends on
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec) return "missing";
    return std::to_string(size) + ' ' + std::to_string((long long)fs::last_write_time(p, ec).time_since_epoch().count());
}

struct Cached {
    std::map<std::string, std::string> files;
    std::vector<merge::ModReport> reports;  // load order
    std::vector<std::string> log;
};
json::Value report_json(const merge::Result& r) {
    json::Value v;
    json::Value& entries = v["mods"];
    entries.type = json::Value::Array;
    for (const auto& m : r.mods) {
        json::Value o;
        o["files"] = double(m.files);
        o["replaced"] = double(m.replaced);
        o["merged"] = double(m.merged);
        o["overridden"] = double(m.overridden);
        json::Value& notes = o["notes"];
        notes.type = json::Value::Array;
        for (const auto& n : m.notes) notes.array.emplace_back(n);
        entries.array.push_back(o);
    }
    json::Value& log = v["log"];
    log.type = json::Value::Array;
    for (const auto& l : r.log) log.array.emplace_back(l);
    return v;
}
bool load_cache(const fs::path& dir, size_t mods, Cached& c) {
    std::error_code ec;
    if (!fs::exists(dir / "complete", ec)) return false;
    {
        std::istringstream in(read_text(dir / "vanilla.txt", 1 << 20));
        std::string line;
        while (std::getline(in, line)) {
            const size_t tab = line.find('\t');
            if (tab == std::string::npos || stamp(line.substr(0, tab)) != line.substr(tab + 1)) return false;
        }
    }
    std::ifstream files(dir / "files.txt", std::ios::binary);
    std::string line;
    while (std::getline(files, line)) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) return false;
        c.files.emplace(line.substr(0, tab), line.substr(tab + 1));
    }
    for (const auto& [key, host] : c.files)
        if (host.rfind(dir.string(), 0) == 0 && !fs::exists(host, ec)) return false;  // a merged file was removed
    try {
        const json::Value v = json::parse(read_text(dir / "report.json", 1 << 20));
        for (const auto& m : v.get("mods").array) {
            merge::ModReport r;
            r.files = size_t(m.get("files").number);
            r.replaced = size_t(m.get("replaced").number);
            r.merged = size_t(m.get("merged").number);
            r.overridden = size_t(m.get("overridden").number);
            for (const auto& n : m.get("notes").array) r.notes.push_back(n.string());
            c.reports.push_back(std::move(r));
        }
        for (const auto& l : v.get("log").array) c.log.push_back(l.string());
    } catch (const std::exception&) {
        return false;
    }
    return c.reports.size() == mods;
}
Cached build_cache(const fs::path& root, const std::string& id, const merge::Vanilla& v, const std::vector<merge::Mod>& inputs) {
    std::error_code ec;
    const fs::path dir = root / id;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "merged", ec);
    if (ec) exception_report::raise("Cannot create the mod cache " + dir.string());
    merge::Result r = merge::build(v, inputs, dir / "merged");
    std::string files, vanilla;
    for (const auto& [key, host] : r.files) files += key + '\t' + host + '\n';
    for (const auto& p : r.vanilla_used) vanilla += p.string() + '\t' + stamp(p) + '\n';
    write_text(dir / "files.txt", files);
    write_text(dir / "vanilla.txt", vanilla);
    write_text(dir / "report.json", json::dump(report_json(r)) + "\n");
    write_text(dir / "complete", "1\n");
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))  // older sets
        if (it->path().filename() != id) fs::remove_all(it->path(), ec);
    return {std::move(r.files), std::move(r.mods), std::move(r.log)};
}

void backup_saves_locked(const std::string& set) {
    const std::string previous = g.saved.last_set;
    if (previous == set) return;
    g.saved.last_set = set;
    save_locked();
    if (previous.empty() && set == "none") return;  // never started with mods
    std::error_code ec;
    const fs::path saves = config::save_dir;
    if (saves.empty() || !fs::is_directory(saves, ec) || fs::is_empty(saves, ec)) return;
    const fs::path root = g.dir.parent_path() / "Save backups";
    char name[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(name, sizeof name, "%Y-%m-%d %H.%M.%S", std::localtime(&now));
    const fs::path dest = root / name;
    fs::create_directories(dest, ec);
    fs::copy(saves, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        LOG("[mods] save backup to %s failed: %s", dest.string().c_str(), ec.message().c_str());
        return;
    }
    LOG("[mods] the set of mods changed: saves backed up to %s", dest.string().c_str());
    std::vector<fs::path> all;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) all.push_back(it->path());
    std::sort(all.begin(), all.end());
    for (size_t k = 0; k + kSaveBackups < all.size(); k++) fs::remove_all(all[k], ec);
}

}  // namespace

std::vector<bool> chosen(const Layout& l, const std::map<std::string, bool>& saved) {
    std::vector<bool> on(l.options.size(), false);
    for (size_t k = 0; k < l.options.size(); k++)
        if (l.options[k].group < 0) {
            auto s = saved.find(l.options[k].key);
            on[k] = s != saved.end() ? s->second : l.options[k].by_default;
        }
    for (int group = 0; group < int(l.groups.size()); group++) {
        long pick = -1, by_default = -1, first = -1;
        bool touched = false;  // the player chose in this group
        for (size_t k = 0; k < l.options.size(); k++) {
            if (l.options[k].group != group) continue;
            if (first < 0) first = long(k);
            if (by_default < 0 && l.options[k].by_default) by_default = long(k);
            auto s = saved.find(l.options[k].key);
            if (s == saved.end()) continue;
            touched = true;
            if (s->second && pick < 0) pick = long(k);
        }
        if (pick < 0 && (!touched || l.groups[size_t(group)].required))
            pick = by_default >= 0 ? by_default : l.groups[size_t(group)].required ? first : -1;
        if (pick >= 0) on[size_t(pick)] = true;
    }
    return on;
}

Layout detect(const fs::path& folder, const std::string& title) {
    Layout l;
    l.name = folder.filename().string();
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        const std::string ext = lower(folder.extension().string());
        l.kind = "File";
        if (ext == ".bnp")
            l.problem = "A BCML mod (.bnp): the Mods folder unpacks these when the game starts";
        else if (ext == ".zip" || ext == ".7z" || ext == ".rar") l.problem = "An archive: unpack it into its own folder here";
        else l.problem = "Not a mod folder";
        return l;
    }
    fs::path cur = folder;
    bool in_title = false;  // inside an SDCafiine title folder: one folder per mod
    for (int depth = 0; depth < 6; depth++) {
        std::vector<fs::path> dirs, files;
        for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec)) {
            if (hidden(it->path().filename().string())) continue;
            (it->is_directory(ec) ? dirs : files).push_back(it->path());
        }
        auto dir = [&](const char* name) {
            for (const auto& d : dirs)
                if (lower(d.filename().string()) == name) return d;
            return fs::path();
        };
        bool switch_mod = !dir("romfs").empty() || !dir("exefs").empty();
        for (const auto& d : dirs)
            if (lower(d.filename().string()).rfind("01007ef00011", 0) == 0) switch_mod = true;  // the Switch title's folders
        if (switch_mod) {
            l.problem = "A mod for the Switch version: it doesn't work with the Wii U game";
            return l;
        }
        fs::path rules;
        for (const auto& f : files)
            if (lower(f.filename().string()) == "rules.txt") rules = f;
        const fs::path content = dir("content"), aoc = dir("aoc");
        if (!rules.empty()) {
            l.kind = "Cemu graphic pack";
            read_rules(rules, l);
            l.code = cemu_files(cur, true);  // its code patches: mods::patches (the translated game code)
            if (cemu_files(cur, false) && !l.code) l.warning = "Its shaders are not used, only its content/ and aoc/ files";
            if (content.empty() && aoc.empty() && !l.code && l.problem.empty())
                l.problem = "This graphic pack has no content/ or aoc/ folder and no code patches (shaders alone are not supported yet)";
            l.content = content;
            l.aoc = aoc;
            return l;
        }
        if (!content.empty() || !aoc.empty()) {
            l.kind = in_title ? "SDCafiine folder" : "Content folder";
            l.content = content;
            l.aoc = aoc;
            std::string unsupported;
            const bool logs = bcml_logs(cur, unsupported);
            std::error_code ec;
            if (logs || fs::exists(cur / "info.json", ec)) {
                l.kind = "BCML mod";
                read_bcml_info(cur, l);
                if (logs) l.logs = cur / "logs";
                if (!unsupported.empty())
                    l.problem = "Part of this mod is BCML merge data that can't be applied yet (logs: " + unsupported +
                                "), and without it the game can crash";
            }
            return l;
        }
        for (const auto& f : files)
            if (lower(f.extension().string()) == ".bnp") {
                l.problem = "It holds a BCML mod (.bnp): move the .bnp itself into the Mods folder";
                return l;
            }
        if (in_title && dirs.size() > 1) {
            l.problem = "This SDCafiine folder holds several mods: move each one into its own folder here";
            return l;
        }
        std::vector<fs::path> titles;
        for (const auto& d : dirs)
            if (title_folder(d.filename().string())) titles.push_back(d);
        if (!titles.empty()) {  // SDCafiine: <title id>/<mod>/content
            fs::path ours;
            for (const auto& t : titles)
                if (lower(t.filename().string()) == lower(title)) ours = t;
            for (const auto& t : titles)
                if (ours.empty() && botw_title(t.filename().string())) ours = t;
            if (ours.empty()) {
                l.problem = "An SDCafiine folder for another game";
                return l;
            }
            cur = ours;
            in_title = true;
            continue;
        }
        if (dirs.size() == 1) {  // a folder around the mod (as unzipped)
            cur = dirs.front();
            continue;
        }
        break;
    }
    l.problem = "No content/ or aoc/ folder in it";
    return l;
}

void startup() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g.started) return;
    g.started = true;
    const char* dir_env = std::getenv("BOTW_MODS_DIR");
    g.dir = dir_env ? fs::path(dir_env) : fs::path(host::config_dir()) / "Mods";
    g.settings = g.dir.parent_path() / "mods.json";
    if (!dir_env && std::getenv("WWHD_NO_HOST_INPUT")) {
        g.off = true;
        g.summary = "Mods are off in headless runs.";
        return;
    }
    std::error_code ec;
    fs::create_directories(g.dir, ec);
    g.saved = load_saved(g.settings);
    scan_locked(true);
    if (const char* e = std::getenv("BOTW_MODS"); e && std::string(e) == "0") {
        g.off = true;
        g.summary = "Mods are off for this run (BOTW_MODS=0).";
        LOG("[mods] %s", g.summary.c_str());
        return;
    }
    const Inputs wanted = wanted_locked();
    if (wanted.empty()) {
        backup_saves_locked("none");
        g.summary = g.layouts.empty() ? "" : "No content mods are switched on.";
        return;
    }
    std::vector<merge::Mod> inputs;  // load order: the bottom of the list first, so the top wins
    for (auto it = wanted.rbegin(); it != wanted.rend(); ++it) {
        const auto& [id, keys] = *it;
        const Layout& l = g.layouts.at(id);
        inputs.push_back({id, l.name, l.content, l.aoc, l.logs});
        for (const auto& o : l.options)  // its chosen options, over the mod (in info.json's order)
            if (std::find(keys.begin(), keys.end(), o.key) != keys.end()) {
                std::error_code ec;
                auto sub = [&](const char* n) { return fs::is_directory(o.folder / n, ec) ? o.folder / n : fs::path(); };
                inputs.push_back({option_id(id, o.key), l.name + ": " + o.name, sub("content"), sub("aoc"), sub("logs"), true});
            }
    }
    const merge::Vanilla vanilla{content_of(config::game_dir), content_of(config::base_dir), content_of(config::aoc_dir)};
    try {
        const auto t0 = std::chrono::steady_clock::now();
        g.cache = cache_root();
        const std::string set = fingerprint(vanilla, inputs);
        Cached c;
        const bool cached = load_cache(g.cache / set, inputs.size(), c);
        if (!cached) c = build_cache(g.cache, set, vanilla, inputs);
        for (size_t k = 0; k < inputs.size(); k++) g.reports[inputs[k].id] = c.reports[k];
        g.active = wanted;
        for (const auto& [key, host] : c.files) g_files.emplace(key, host);
        g_ready.store(!g_files.empty(), std::memory_order_release);
        std::string text;
        for (const auto& l : c.log) text += (text.empty() ? "" : ". ") + l;
        g.summary = text + ".";
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        LOG("[mods] %s (%s in %.0f ms; %s)", g.summary.c_str(), cached ? "from the cache" : "merged", ms, (g.cache / set).string().c_str());
        backup_saves_locked(set);
    } catch (const std::exception& e) {
        g_files.clear();
        g.active.clear();
        g.reports.clear();
        g.summary = std::string("Mods are off this run: ") + e.what();
        LOG("[mods] %s", g.summary.c_str());
    }
}

std::string replacement(const std::string& guest) {
    if (!g_ready.load(std::memory_order_acquire)) return {};
    std::string key;
    if (guest.rfind("/vol/content/", 0) == 0) key = "content/" + guest.substr(13);
    else if (guest.rfind("/vol/aoc", 0) == 0) {
        const size_t slash = guest.find('/', 8);
        if (slash == std::string::npos) return {};
        key = "aoc/" + guest.substr(slash + 1);
    } else if (!guest.empty() && guest[0] != '/') key = "content/" + guest;
    else return {};
    auto it = g_files.find(lower(key));
    return it == g_files.end() ? std::string() : it->second;
}

std::vector<ModView> list() {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<ModView> out;
    for (const auto& [id, on] : g.saved.order) {
        auto it = g.layouts.find(id);
        if (it == g.layouts.end()) continue;
        const Layout& l = it->second;
        ModView v;
        v.id = id;
        v.name = l.name;
        v.kind = l.kind;
        v.description = l.description;
        v.enabled = on;
        v.problem = l.problem;
        v.warning = l.warning;
        v.code = l.code;
        v.content = merged(l);
        const std::vector<std::string>* active = active_locked(id);
        v.active = active != nullptr;
        if (auto r = g.reports.find(id); r != g.reports.end() && v.active) {
            v.files = r->second.files;
            v.used = r->second.replaced + r->second.merged;
            v.notes = r->second.notes;
        }
        if (v.active && !l.aoc.empty() && config::aoc_dir.empty()) v.notes.push_back("The DLC isn't installed, so its aoc/ files are not used");
        if (l.problem.empty()) {
            v.groups = l.groups;
            const std::vector<bool> use = chosen(l, choices_locked(id));
            for (size_t k = 0; k < l.options.size(); k++) {
                const Layout::Option& o = l.options[k];
                OptionView ov;
                ov.key = o.key;
                ov.name = o.name;
                ov.description = o.description;
                ov.group = o.group;
                ov.on = use[k];
                ov.active = active && std::find(active->begin(), active->end(), o.key) != active->end();
                if (auto r = g.reports.find(option_id(id, o.key)); r != g.reports.end() && ov.active) {
                    ov.files = r->second.files;
                    ov.used = r->second.replaced + r->second.merged;
                    ov.notes = r->second.notes;
                }
                v.options.push_back(std::move(ov));
            }
        }
        out.push_back(std::move(v));
    }
    return out;
}
std::string directory() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g.dir.string();
}
std::string summary() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g.summary;
}
bool restart_needed() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g.started && !g.off && wanted_locked() != g.active;
}
void set_enabled(const std::string& id, bool on) {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& entry : g.saved.order)
        if (entry.first == id && entry.second != on) {
            entry.second = on;
            save_locked();
            LOG("[mods] %s %s (from the next start)", id.c_str(), on ? "on" : "off");
        }
}
void set_option(const std::string& id, const std::string& key, bool on) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g.layouts.find(id);
    if (it == g.layouts.end()) return;
    const Layout& l = it->second;
    auto o = std::find_if(l.options.begin(), l.options.end(), [&](const Layout::Option& x) { return x.key == key; });
    if (o == l.options.end()) return;
    Choices next = choices_locked(id);
    if (o->group < 0) {
        next[key] = on;
    } else {  // the group's choice: this option or none; the group's other options off
        if (!on && l.groups[size_t(o->group)].required) return;
        for (const auto& x : l.options)
            if (x.group == o->group) next[x.key] = on && x.key == key;
    }
    if (next == choices_locked(id)) return;
    g.saved.options[id] = std::move(next);
    save_locked();
    LOG("[mods] %s: option %s %s (from the next start)", id.c_str(), key.c_str(), on ? "on" : "off");
}
void move(const std::string& id, int delta) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto& order = g.saved.order;
    for (size_t i = 0; i < order.size(); i++)
        if (order[i].first == id) {
            const long j = long(i) + delta;
            if (j < 0 || j >= long(order.size())) return;
            std::swap(order[i], order[size_t(j)]);
            save_locked();
            return;
        }
}
void rescan() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g.started || g.dir.empty()) return;
    std::error_code ec;
    fs::create_directories(g.dir, ec);
    scan_locked();
}

}  // namespace mods::loader
