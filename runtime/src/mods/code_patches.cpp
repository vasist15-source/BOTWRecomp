// Code mods: see code_patches.h. The game code reads botw_patch_switch[] and botw_patch_values[]
// (botw_patches.h); the manifest and the caves' functions come from build/gen/code_patches.c.
#include "code_patches.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include "../platform/host.h"
#include "../platform/process.h"
#include "../runtime.h"
#include "botw_patches.h"
#include "code_patch_rules.h"
#include "mod_json.h"

#ifndef _WIN32
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

uint8_t botw_patch_switch[BOTW_PATCH_MAX_MODS] = {};      // C linkage from botw_patches.h
uint32_t botw_patch_values[BOTW_PATCH_MAX_VALUES] = {};

namespace mods::patches {
namespace fs = std::filesystem;
namespace {

struct Item {  // a write to guest memory: bytes, or a value of `kind` computed by `program`
    uint32_t addr = 0;
    std::string bytes, kind;
    json::Value program;
    size_t size() const {
        if (kind.empty()) return bytes.size();
        return kind == "double" ? 8 : kind == "u16" ? 2 : kind == "u8" ? 1 : 4;
    }
};
struct Mod {  // a pack the game code holds (the manifest)
    std::string id, name, description, fingerprint, problem;
    int sw = -1;
    Rules rules;
    std::vector<Item> memory, writes, data;  // cave bytes, cave data from presets, game data while on
    std::vector<std::pair<int, json::Value>> values;  // instruction immediates from presets
    size_t sites = 0;
    std::vector<std::string> notes;
    std::map<std::string, std::string> chosen;  // category -> preset
    Selection selection;
    bool on = false;
    std::vector<std::string> saved;  // the game's own bytes under `data` while on
};
struct Plan {  // how this game rebuilds its code
    std::vector<std::vector<std::string>> steps;
    std::vector<std::vector<std::string>> envs;  // per step: more environment (NAME=value)
    std::vector<double> weights;  // of each step in the progress
    bool installer = false;       // the steps print the installer's JSON events
    std::string exe, how;
};
struct Rebuild {
    std::thread thread;
    pid_t child = 0;
    bool cancelled = false;
    ~Rebuild();
};
struct State {
    bool started = false, off = false;
    fs::path dir, settings;
    std::vector<std::string> args;
    std::vector<Mod> mods;                // the game code's packs
    std::map<std::string, Build> folder;  // the Mods folder's code mods: id -> how the game code holds them
    RebuildState rebuild;
    bool planned = false, plan_ok = false;  // make_plan's result, once
    Plan plan;
    std::string plan_why;
};
std::mutex g_mutex;
State g;
Rebuild g_rebuild;  // after g: destroyed first

std::string unhex(const std::string& s) {
    std::string out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out += char(std::stoi(s.substr(i, 2), nullptr, 16));
    return out;
}
bool guest_range(uint32_t a, size_t n) { return a >= 0x10000 && uint64_t(a) + n <= 0x100000000ull && n <= (1u << 24); }
void write_bytes(uint32_t a, const std::string& bytes) {
    if (guest_range(a, bytes.size())) memcpy(mem::ptr(a), bytes.data(), bytes.size());
}
// a value as tools/cemu_patches.py store_reloc writes it: big-endian, (float) / (uint32) conversions
void write_value(const Item& d, const std::map<std::string, double>& vars) {
    const double v = evaluate(d.program, vars);
    uint8_t b[8];
    const size_t n = d.size();
    if (d.kind == "float") {
        const float f = (float)v;
        uint32_t u;
        memcpy(&u, &f, 4);
        for (int i = 0; i < 4; i++) b[i] = uint8_t(u >> (24 - 8 * i));
    } else if (d.kind == "double") {
        uint64_t u;
        memcpy(&u, &v, 8);
        for (int i = 0; i < 8; i++) b[i] = uint8_t(u >> (56 - 8 * i));
    } else {
        const uint32_t u = to_u32(v);
        for (size_t i = 0; i < n; i++) b[i] = uint8_t(u >> (8 * (n - 1 - i)));
    }
    if (guest_range(d.addr, n)) memcpy(mem::ptr(d.addr), b, n);
}
Item item_of(const json::Value& v) {
    Item it;
    it.addr = uint32_t(v.array.at(0).number);
    if (v.array.size() == 2) it.bytes = unhex(v.array[1].string());
    else {
        it.kind = v.array.at(1).string();
        it.program = v.array.at(2);
    }
    return it;
}

void parse_manifest_locked() {
    const std::string text = g_botw_patch_manifest;
    if (text.empty()) return;
    const json::Value m = json::parse(text);
    for (const json::Value& e : m.get("mods").array) {
        Mod mod;
        mod.id = e.get("id").string();
        mod.name = e.get("name").string(mod.id);
        mod.description = e.get("description").string();
        mod.fingerprint = e.get("fingerprint").string();
        mod.problem = e.get("problem").string();
        for (const auto& n : e.get("notes").array) mod.notes.push_back(n.string());
        if (mod.problem.empty()) {
            mod.sw = int(e.get("switch").number);
            if (mod.sw < 0 || mod.sw >= BOTW_PATCH_MAX_MODS) exception_report::raise("Code mod switch out of range");
            mod.rules = rules_of(e);
            for (const auto& c : e.get("memory").array) mod.memory.push_back(item_of(c));
            for (const auto& c : e.get("writes").array) mod.writes.push_back(item_of(c));
            for (const auto& c : e.get("data").array) mod.data.push_back(item_of(c));
            for (const auto& c : e.get("values").array) {
                const int slot = int(c.array.at(0).number);
                if (slot < 0 || slot >= BOTW_PATCH_MAX_VALUES) exception_report::raise("Code mod value out of range");
                mod.values.push_back({slot, c.array.at(1)});
            }
            mod.sites = e.get("sites").array.size();
        }
        g.mods.push_back(std::move(mod));
    }
}

std::map<std::string, std::map<std::string, std::string>> load_settings() {
    std::map<std::string, std::map<std::string, std::string>> out;
    std::ifstream f(g.settings, std::ios::binary);
    if (!f) return out;
    try {
        const json::Value v = json::parse(std::string{std::istreambuf_iterator<char>(f), {}});
        for (const auto& [id, cats] : v.get("presets").object)
            for (const auto& [cat, name] : cats.object) out[id][cat] = name.string();
    } catch (const std::exception& e) {
        LOG("[code mods] %s: %s (presets reset)", g.settings.string().c_str(), e.what());
    }
    return out;
}
void save_settings_locked() {
    json::Value v;
    v["format_version"] = 1;
    json::Value& presets = v["presets"];
    presets.type = json::Value::Object;
    for (const Mod& m : g.mods)
        if (!m.chosen.empty())
            for (const auto& [cat, name] : m.chosen) presets[m.id][cat] = name;
    const fs::path tmp = g.settings.string() + ".tmp";
    std::error_code ec;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << json::dump(v) << "\n";
        if (!f) {
            LOG("[code mods] cannot write %s", tmp.string().c_str());
            return;
        }
    }
    fs::rename(tmp, g.settings, ec);
}

Mod* built_locked(const std::string& id) {
    for (Mod& m : g.mods)
        if (m.id == id && m.problem.empty()) return &m;
    return nullptr;
}
const Mod* entry_locked(const std::string& id) {
    for (const Mod& m : g.mods)
        if (m.id == id) return &m;
    return nullptr;
}

// the chosen presets' values: cave data, immediates, and the game data while on
void refresh_locked(Mod& m) {
    m.selection = select(m.rules, m.chosen);
    const auto vars = values(m.rules, m.selection);
    try {
        for (const Item& w : m.writes) write_value(w, vars);
        for (const auto& [slot, program] : m.values)
            __atomic_store_n(&botw_patch_values[slot], to_u32(evaluate(program, vars)), __ATOMIC_RELAXED);
        if (m.on)
            for (const Item& d : m.data)
                if (!d.kind.empty()) write_value(d, vars);
    } catch (const std::exception& e) {
        LOG("[code mods] %s: %s", m.id.c_str(), e.what());
    }
}
void switch_on_locked(Mod& m) {
    if (m.on) return;
    m.saved.clear();
    for (const Item& d : m.data)
        m.saved.push_back(guest_range(d.addr, d.size()) ? std::string((const char*)mem::ptr(d.addr), d.size()) : std::string());
    const auto vars = values(m.rules, m.selection);
    for (const Item& d : m.data) {
        if (d.kind.empty()) write_bytes(d.addr, d.bytes);
        else {
            try {
                write_value(d, vars);
            } catch (const std::exception& e) {
                LOG("[code mods] %s: %s", m.id.c_str(), e.what());
            }
        }
    }
    m.on = true;
    __atomic_store_n(&botw_patch_switch[m.sw], 1, __ATOMIC_RELEASE);
}
void switch_off_locked(Mod& m) {
    if (!m.on) return;
    __atomic_store_n(&botw_patch_switch[m.sw], 0, __ATOMIC_RELEASE);
    m.on = false;
    for (size_t i = m.data.size(); i-- > 0;)
        if (i < m.saved.size() && m.saved[i].size() == m.data[i].size()) write_bytes(m.data[i].addr, m.saved[i]);
    m.saved.clear();
}

bool hidden(const std::string& name) {
    return name.empty() || name[0] == '.' || name == "__MACOSX" || name == "Thumbs.db" || name == "desktop.ini";
}
// how the game code holds the Mods folder's code mods (tools/botw_code_mods.py find_packs)
void scan_locked() {
    g.folder.clear();
    std::error_code ec;
    for (fs::directory_iterator it(g.dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string id = it->path().filename().string();
        std::error_code dec;
        if (hidden(id) || !fs::is_directory(it->path(), dec)) continue;
        const fs::path pack = pack_folder(it->path());
        if (pack.empty() || pack_files(pack).size() < 2) continue;  // rules.txt and no patch files
        std::string print;
        try {
            print = fingerprint(pack);
        } catch (const std::exception& e) {
            LOG("[code mods] %s: %s", id.c_str(), e.what());
        }
        const Mod* m = entry_locked(id);
        if (!m) g.folder[id] = Build::Missing;
        else if (m->fingerprint != print) g.folder[id] = Build::Changed;
        else g.folder[id] = m->problem.empty() ? Build::Current : Build::Refused;
    }
}
Build folder_state_locked(const std::string& id) {
    auto it = g.folder.find(id);
    return it == g.folder.end() ? Build::Missing : it->second;
}

View view_locked(const std::string& id) {
    View v;
    v.id = v.name = id;
    v.build = folder_state_locked(id);
    if (const Mod* m = entry_locked(id)) {
        v.name = m->name;
        v.description = m->description;
        v.notes = m->notes;
        if (!m->problem.empty() && v.build == Build::Refused) v.problem = m->problem;
        v.on = m->on;
        v.changes = m->sites;
        if (m->problem.empty() && v.build == Build::Current) {
            for (const std::string& category : m->rules.categories) {
                Category c;
                c.name = category;
                for (size_t i = 0; i < m->rules.presets.size(); i++) {
                    const Preset& p = m->rules.presets[i];
                    if (p.category != category || !m->selection.visible[i]) continue;
                    if (m->selection.active[i]) c.active = int(c.presets.size());
                    c.presets.push_back(p.name);
                }
                if (!c.presets.empty()) v.categories.push_back(std::move(c));
            }
        }
    }
    return v;
}

}  // namespace

void startup(int argc, char** argv, const fs::path& dir, const std::function<bool(const std::string&)>& enabled) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g.started) return;
    g.started = true;
    g.args.assign(argv, argv + argc);
    g.dir = dir;
    g.settings = dir.parent_path() / "code_mods.json";
    for (unsigned i = 0; i < g_botw_patch_func_count; i++)
        if (g_botw_patch_funcs[i].fn) dispatch::set(g_botw_patch_funcs[i].addr, g_botw_patch_funcs[i].fn);
    try {
        parse_manifest_locked();
    } catch (const std::exception& e) {
        LOG("[code mods] the game code's list of code mods can't be read: %s", e.what());
        g.mods.clear();
    }
    const char* env = std::getenv("BOTW_MODS");
    g.off = (env && std::string(env) == "0") || (!std::getenv("BOTW_MODS_DIR") && std::getenv("WWHD_NO_HOST_INPUT"));
    const auto saved = load_settings();
    scan_locked();
    std::string on, off;
    for (Mod& m : g.mods) {
        if (!m.problem.empty()) continue;
        for (const Item& c : m.memory) write_bytes(c.addr, c.bytes);
        if (auto it = saved.find(m.id); it != saved.end()) m.chosen = it->second;
        refresh_locked(m);
        if (!g.off && folder_state_locked(m.id) == Build::Current && enabled(m.id)) {
            switch_on_locked(m);
            on += (on.empty() ? "" : ", ") + m.id;
        } else {
            off += (off.empty() ? "" : ", ") + m.id;
        }
    }
    size_t pending = 0;
    for (const auto& [id, b] : g.folder) pending += b == Build::Missing || b == Build::Changed;
    if (!g.mods.empty() || !g.folder.empty())
        LOG("[code mods] %zu in the game code (%zu functions in code caves); on: %s; off: %s%s; %zu to build in",
            g.mods.size(), (size_t)g_botw_patch_func_count, on.empty() ? "none" : on.c_str(), off.empty() ? "none" : off.c_str(),
            g.off ? " (mods are off this run)" : "", pending);
}

std::vector<View> list() {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<View> out;
    for (const auto& [id, b] : g.folder) out.push_back(view_locked(id));
    return out;
}
bool find(const std::string& id, View& out) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g.folder.count(id)) return false;
    out = view_locked(id);
    return true;
}
bool is_code_mod(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g.folder.count(id) != 0;
}
void set_on(const std::string& id, bool on) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Mod* m = built_locked(id);
    if (!m || m->on == on || (on && folder_state_locked(id) != Build::Current)) return;  // off always works
    if (on) switch_on_locked(*m);
    else switch_off_locked(*m);
    LOG("[code mods] %s %s", id.c_str(), on ? "on" : "off");
}
void set_preset(const std::string& id, const std::string& category, const std::string& preset) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Mod* m = built_locked(id);
    if (!m) return;
    auto it = m->chosen.find(category);
    if (it != m->chosen.end() && it->second == preset) return;
    m->chosen[category] = preset;
    refresh_locked(*m);
    save_settings_locked();
    LOG("[code mods] %s: %s = %s", id.c_str(), category.c_str(), preset.c_str());
}
void rescan() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g.started) scan_locked();
}
bool rebuild_needed() {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (const auto& [id, b] : g.folder)
        if (b == Build::Missing || b == Build::Changed) return true;
    return false;
}

// ---- rebuilding the game code

namespace {
std::string cache_var(const fs::path& cache, const std::string& key) {  // KEY:TYPE=value of CMakeCache.txt
    std::ifstream f(cache);
    std::string line;
    while (std::getline(f, line))
        if (line.rfind(key + ":", 0) == 0) {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) return line.substr(eq + 1);
        }
    return {};
}
bool make_plan(Plan& p, std::string& why) {
#ifndef __APPLE__
    why = "Rebuilding the game code is only set up on macOS.";
    return false;
#else
    std::error_code ec;
    const fs::path exe_dir = host::exe_dir();
    const fs::path python = fs::exists("/usr/bin/python3", ec) ? "/usr/bin/python3" : "python3";
    const fs::path app = exe_dir.parent_path().parent_path();
    const bool bundle = exe_dir.filename() == "MacOS" && app.extension() == ".app";
    // an app tools/make_app.sh made from a source checkout: it starts in that checkout (BotWLaunch's dir)
    const fs::path checkout = fs::current_path(ec);
    const bool made_app = bundle && fs::exists(checkout / "tools" / "make_app.sh", ec) &&
                          fs::exists(checkout / "build" / "rt" / "CMakeCache.txt", ec);
    if (fs::exists(exe_dir / "CMakeCache.txt", ec) || made_app) {  // a build folder of a source checkout
        const fs::path build = made_app ? checkout / "build" / "rt" : exe_dir;
        const fs::path cache = build / "CMakeCache.txt";
        const fs::path src = cache_var(cache, "CMAKE_HOME_DIRECTORY"), gen = cache_var(cache, "GEN_DIR");
        const std::string cmake = cache_var(cache, "CMAKE_COMMAND");
        const fs::path setup = src / "tools" / "installer" / "botw_setup.py";
        const fs::path rpx = fs::path(config::game_dir) / "code" / "U-King.rpx";
        if (!fs::exists(setup, ec) || gen.empty() || cmake.empty() || !fs::exists(rpx, ec)) {
            why = "The build folder " + build.string() + " doesn't say where its source and generated code are.";
            return false;
        }
        p.steps = {{python.string(), "-u", setup.string(), "recomp", rpx.string(), gen.string()},
                   {cmake, "--build", build.string(), "--target", "wwhd"}};
        p.envs = {{}, {}};
        p.weights = {0.75, 0.25};
        p.exe = (exe_dir / fs::path(g.args.empty() ? "wwhd" : g.args[0]).filename()).string();
        p.how = "Translates the game code into " + gen.string() + " and builds " + (build / "wwhd").string() + ".";
        if (made_app) {  // then bundles it again where this app is, with the same game folders
            p.steps.push_back({"/bin/sh", (src / "tools" / "make_app.sh").string(), app.parent_path().string()});
            p.envs.push_back({"BOTW_DUMP=" + fs::path(config::game_dir).parent_path().string()});
            p.weights = {0.7, 0.25, 0.05};
            p.how += " Then tools/make_app.sh makes " + app.string() + " again.";
        }
        return true;
    }
    const fs::path plist = fs::path(host::config_dir()) / "install.plist";
    if (!bundle || !fs::exists(plist, ec)) {
        why = "This copy of the game wasn't made by the installer or built from source here, so it can't rebuild itself.";
        return false;
    }
    const host::ProcessResult r = host::run_process({"/usr/bin/plutil", "-convert", "json", "-o", "-", plist.string()});
    json::Value cfg;
    try {
        cfg = json::parse(r.output);
    } catch (const std::exception&) {
        why = "The installer's settings (" + plist.string() + ") can't be read.";
        return false;
    }
    const fs::path setup = fs::path(cfg.get("source").string()) / "tools" / "installer" / "botw_setup.py";
    if (!fs::equivalent(fs::path(cfg.get("app").string()), app, ec) || !fs::exists(setup, ec)) {
        why = "The installer that made this game (" + cfg.get("source").string() + ") isn't there any more.";
        return false;
    }
    std::vector<std::string> cmd = {"/usr/bin/python3", "-u", setup.string(), "--json", "install", "--base", cfg.get("base").string(),
                                    "--update", cfg.get("update").string()};
    if (!cfg.get("dlc").string().empty()) cmd.insert(cmd.end(), {"--dlc", cfg.get("dlc").string()});
    cmd.insert(cmd.end(), {"--dest", app.parent_path().string(), "--keep-build"});
    p.steps = {cmd};
    p.weights = {1.0};
    p.installer = true;
    p.exe = (app / "Contents" / "MacOS" / "wwhd").string();
    p.how = "Runs the installer again (" + setup.string() + "), which updates " + app.string() + ".";
    const char* home = std::getenv("HOME");
    if (!fs::exists(fs::path(home ? home : ".") / "Library" / "Caches" / "BotW" / "installer" / "gen" / "funcs.h", ec))
        p.how += " The installer didn't keep the game's build, so the first rebuild compiles all of it (10-20 minutes); "
                 "later ones only what changed.";
    return true;
#endif
}

const Plan* plan_locked(std::string& why) {
    if (!g.planned) {
        g.planned = true;
        g.plan_ok = make_plan(g.plan, g.plan_why);
    }
    why = g.plan_why;
    return g.plan_ok ? &g.plan : nullptr;
}

void update(const std::function<void(RebuildState&)>& f) {
    std::lock_guard<std::mutex> lk(g_mutex);
    f(g.rebuild);
}

#ifndef _WIN32
// runs one step, handing each line of its output (stdout and stderr) to on_line; the exit code, or -1
int run_step(const std::vector<std::string>& args, const std::vector<std::string>& more_env, const std::string& mods_dir,
             const std::function<void(const std::string&)>& on_line, std::string& error) {
    int pipefd[2];
    if (pipe(pipefd)) {
        error = "can't create a pipe";
        return -1;
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);  // its own group: a cancel stops all of it
    posix_spawnattr_setpgroup(&attr, 0);
    std::vector<std::string> env;
    std::vector<std::string> set = more_env;
    set.push_back("BOTW_MODS_DIR=" + mods_dir);
    for (char** e = environ; *e; e++) {
        const char* eq = std::strchr(*e, '=');
        const std::string name(*e, eq ? size_t(eq - *e) + 1 : std::strlen(*e));
        if (std::none_of(set.begin(), set.end(), [&](const std::string& s) { return s.rfind(name, 0) == 0; })) env.push_back(*e);
    }
    env.insert(env.end(), set.begin(), set.end());
    std::vector<char*> envp, argv;
    for (auto& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);
    std::vector<std::string> a = args;
    for (auto& s : a) argv.push_back(s.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        error = "can't start " + args[0] + " (" + std::strerror(rc) + ")";
        return -1;
    }
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_rebuild.child = pid;
    }
    std::string pending;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(pipefd[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        pending.append(buf, size_t(n));
        for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
            on_line(pending.substr(0, nl));
            pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty()) on_line(pending);
    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_rebuild.child = 0;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

void rebuild_worker(Plan p, std::string mods_dir) {
#ifndef _WIN32
    double base = 0;
    std::vector<std::string> tail;
    for (size_t k = 0; k < p.steps.size(); k++) {
        const double weight = p.weights[k];
        LOG("[code mods] rebuild: %s", p.steps[k].front().c_str());
        std::string installer_error;
        auto on_line = [&](const std::string& line) {
            tail.push_back(line);
            if (tail.size() > 30) tail.erase(tail.begin());
            double f = -1;
            std::string stage;
            if (p.installer && !line.empty() && line[0] == '{') {
                try {
                    const json::Value ev = json::parse(line);
                    const std::string kind = ev.get("event").string();
                    if (kind == "progress") f = ev.get("overall").number, stage = ev.get("detail").string();
                    else if (kind == "step" && ev.get("state").string() == "running") stage = ev.get("title").string();
                    else if (kind == "error") installer_error = ev.get("message").string();
                    else if (kind == "log") log_msg("[code mods] %s", ev.get("text").string().c_str());
                } catch (const std::exception&) {
                }
            } else if (line.rfind("@progress ", 0) == 0) {  // botw_setup.py recomp
                unsigned n = 0, total = 0;
                char phase[16] = {};
                if (std::sscanf(line.c_str(), "@progress %15s %u %u", phase, &n, &total) == 3) {
                    f = std::string(phase) == "emit" && total ? 0.3 + 0.7 * n / total : 0.15;
                    stage = std::string(phase) == "emit" ? "Translating the game code" : "Analysing the game code";
                }
            } else if (line.size() > 1 && line[0] == '[') {  // ninja: [n/total] ...
                unsigned n = 0, total = 0;
                if (std::sscanf(line.c_str(), "[%u/%u]", &n, &total) == 2 && total) {
                    f = double(n) / total;
                    stage = "Compiling the game";
                }
                log_msg("[code mods] %s", line.c_str());
            } else {
                log_msg("[code mods] %s", line.c_str());
            }
            if (f >= 0 || !stage.empty())
                update([&](RebuildState& s) {
                    if (f >= 0) s.progress = base + weight * std::min(1.0, std::max(0.0, f));
                    if (!stage.empty()) s.stage = stage;
                });
        };
        std::string error;
        const int code = run_step(p.steps[k], k < p.envs.size() ? p.envs[k] : std::vector<std::string>{}, mods_dir, on_line, error);
        bool cancelled;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            cancelled = g_rebuild.cancelled;
        }
        if (code != 0 || cancelled) {
            std::string message = cancelled ? "Cancelled; the game code is as it was." : !installer_error.empty() ? installer_error : error;
            if (message.empty()) {
                message = "The rebuild failed (" + fs::path(p.steps[k].front()).filename().string() + ", exit code " +
                          std::to_string(code) + ")";
                for (size_t i = tail.size() > 6 ? tail.size() - 6 : 0; i < tail.size(); i++) message += "\n" + tail[i];
            }
            LOG("[code mods] rebuild: %s", message.c_str());
            update([&](RebuildState& s) {
                s.running = false;
                s.error = message;
            });
            return;
        }
        base += weight;
    }
    LOG("[code mods] rebuild done: %s", p.exe.c_str());
    update([&](RebuildState& s) {
        s.running = false;
        s.done = true;
        s.progress = 1;
        s.stage = "Done";
    });
#endif
}

Rebuild::~Rebuild() {
    if (!thread.joinable()) return;
#ifndef _WIN32
    if (child > 0) kill(-child, SIGTERM);
#endif
    cancelled = true;
    thread.join();
}
}  // namespace

RebuildState rebuild_state() {
    std::lock_guard<std::mutex> lk(g_mutex);
    RebuildState s = g.rebuild;
    std::string why;
    const Plan* p = plan_locked(why);
    s.possible = p != nullptr;
    if (s.how.empty()) s.how = p ? p->how : why;
    return s;
}

void rebuild_start() {
    std::string why;
    std::unique_lock<std::mutex> lk(g_mutex);
    if (g.rebuild.running) return;
    const Plan* planned = plan_locked(why);
    if (!planned) {
        g.rebuild.error = why;
        return;
    }
    const Plan p = *planned;
    if (g_rebuild.thread.joinable()) {
        lk.unlock();
        g_rebuild.thread.join();
        lk.lock();
    }
    g.rebuild = RebuildState{};
    g.rebuild.running = true;
    g.rebuild.possible = true;
    g.rebuild.how = p.how;
    g.rebuild.stage = "Starting";
    g_rebuild.cancelled = false;
    LOG("[code mods] rebuilding the game code: %s", p.how.c_str());
    g_rebuild.thread = std::thread(rebuild_worker, p, g.dir.string());
}

void rebuild_cancel() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g.rebuild.running) return;
    g_rebuild.cancelled = true;
#ifndef _WIN32
    if (g_rebuild.child > 0) kill(-g_rebuild.child, SIGTERM);
#endif
}

}  // namespace mods::patches
