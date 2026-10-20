// Bundled Cemu graphic packs (graphic_packs.h): rules.txt presets and Metal shader replacements
#include "graphic_packs.h"
#include "graphic_packs_data.h"
#include "cemu_pack.h"
#include "../overlay/hostui.h"
#include "../runtime.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <sstream>
#include <tuple>

namespace graphic_packs {
namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

struct Var {
    std::string expr;
    bool isInt = false;
};
struct Preset {
    std::string name, category;
    std::map<std::string, Var> vars;  // "$name" -> value
};
struct ShaderFile {
    uint64_t base, aux;
    bool vertex;
    const char* text;
};
struct Loaded {
    Pack info;
    std::map<std::string, Var> defaults;
    std::vector<Preset> presets;
    std::vector<ShaderFile> shaders;
};

// categories the port has its own setting for (Settings > Graphics: 16x anisotropic filtering)
bool hidden(const std::string& category) { return category == "Anisotropic Filtering"; }

// "$name" or "$name:int" = value
void put_var(std::map<std::string, Var>& vars, const std::string& key, const std::string& value) {
    std::string name = key;
    bool isInt = false;
    if (size_t colon = key.find(':'); colon != std::string::npos) {
        name = trim(key.substr(0, colon));
        isInt = lower(trim(key.substr(colon + 1))) == "int";
    }
    vars[name] = Var{value, isInt};
}

void parse_rules(Loaded& p, const std::string& text) {
    std::istringstream in(text);
    std::string line, section;
    Preset* preset = nullptr;
    bool presetDefault = false;
    auto finish = [&] {
        if (!preset) return;
        if (preset->category.empty()) preset->category = "Preset";
        Category* cat = nullptr;
        for (auto& c : p.info.categories) if (c.name == preset->category) cat = &c;
        if (!cat && !hidden(preset->category)) {
            p.info.categories.push_back({preset->category, {}, {}});
            cat = &p.info.categories.back();
        }
        if (cat) {
            cat->presets.push_back(preset->name);
            if (presetDefault || cat->fallback.empty()) cat->fallback = presetDefault ? preset->name : cat->presets.front();
        }
        preset = nullptr;
        presetDefault = false;
    };
    while (std::getline(in, line)) {
        if (size_t hash = line.find('#'); hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            finish();
            section = lower(trim(line.substr(1, line.size() - 2)));
            if (section == "preset") {
                p.presets.push_back({});
                preset = &p.presets.back();
            }
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (section == "definition") {
            if (lower(key) == "name") p.info.name = unquote(value);
            else if (lower(key) == "description") p.info.description = unquote(value);
        } else if (section == "default") {
            if (key.starts_with("$")) put_var(p.defaults, key, value);
        } else if (section == "preset" && preset) {
            if (key.starts_with("$")) put_var(preset->vars, key, value);
            else if (lower(key) == "name") preset->name = unquote(value);
            else if (lower(key) == "category") preset->category = unquote(value);
            else if (lower(key) == "default") presetDefault = value == "1" || lower(value) == "true";
        }
        // [TextureRedefine]: only overwriteAnisotropy in the bundled packs (the port's own setting)
    }
    finish();
}

std::vector<Loaded>& loaded() {
    static std::vector<Loaded> all = [] {
        std::map<std::string, Loaded> byDir;
        for (size_t i = 0; i < kFileCount; i++) {
            std::string path = kFiles[i].path;
            size_t slash = path.find('/');
            if (slash == std::string::npos) continue;
            std::string dir = path.substr(0, slash), name = lower(path.substr(slash + 1));
            Loaded& p = byDir[dir];
            p.info.id = dir;
            unsigned long long base, aux;
            char stage[3] = {};
            if (name == "rules.txt") parse_rules(p, kFiles[i].text);
            else if (sscanf(name.c_str(), "%16llx_%16llx_%2[vps]_msl.txt", &base, &aux, stage) == 3)
                p.shaders.push_back({base, aux, stage[0] == 'v', kFiles[i].text});
        }
        std::vector<Loaded> v;
        for (auto& [dir, p] : byDir) v.push_back(std::move(p));
        return v;
    }();
    return all;
}

std::string setting_key(const Pack& pack, const Category& category) { return "graphicPack." + pack.id + "." + category.name; }

// the choices this session's shaders use, and the sources built from them
struct Active {
    std::map<std::string, std::string> choices;  // setting key -> preset
    std::map<std::tuple<uint64_t, uint64_t, bool>, std::string> shaders;
};

std::string format(double v, bool isInt) {
    if (isInt) return std::to_string((long long)std::llround(v));
    char b[64];
    snprintf(b, sizeof b, "%.9g", v);
    std::string s = b;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

// $name -> value (identifier boundary; unknown names are left as they are)
std::string expand(const char* text, const std::map<std::string, std::pair<double, bool>>& vals) {
    std::string out;
    for (const char* p = text; *p;) {
        if (*p != '$') { out += *p++; continue; }
        const char* e = p + 1;
        while (isalnum((unsigned char)*e) || *e == '_') e++;
        auto it = vals.find(std::string(p, e));
        if (it == vals.end()) { out.append(p, e); p = e; continue; }
        out += format(it->second.first, it->second.second);
        p = e;
    }
    return out;
}

const Active& active() {
    static Active a = [] {
        Active a;
        for (auto& p : loaded()) {
            std::map<std::string, Var> vars = p.defaults;
            for (auto& cat : p.info.categories) {
                std::string choice = selected(p.info, cat);
                a.choices[setting_key(p.info, cat)] = choice;
                for (auto& pr : p.presets)
                    if (pr.category == cat.name && pr.name == choice)
                        for (auto& [k, v] : pr.vars) vars[k] = v;
            }
            // values may refer to each other: resolve in passes
            std::map<std::string, double> resolved;
            std::map<std::string, std::pair<double, bool>> vals;
            for (size_t pass = 0; pass <= vars.size() && resolved.size() < vars.size(); pass++)
                for (auto& [k, v] : vars) {
                    if (resolved.count(k)) continue;
                    try {
                        double d = mods::cemu::expression(v.expr, resolved);
                        resolved[k] = d;
                        vals[k] = {d, v.isInt};
                    } catch (const std::exception&) {
                    }
                }
            for (auto& [k, v] : vars)
                if (!resolved.count(k)) LOG("[gpack] %s: variable %s = %s not resolved", p.info.id.c_str(), k.c_str(), v.expr.c_str());
            for (auto& s : p.shaders) a.shaders[{s.base, s.aux, s.vertex}] = expand(s.text, vals);
            std::string summary;
            for (auto& cat : p.info.categories) summary += " | " + cat.name + ": " + a.choices[setting_key(p.info, cat)];
            LOG("[gpack] %s: %zu shaders%s", p.info.id.c_str(), p.shaders.size(), summary.c_str());
        }
        return a;
    }();
    return a;
}

}  // namespace

const std::vector<Pack>& packs() {
    static std::vector<Pack> v = [] {
        std::vector<Pack> r;
        for (auto& p : loaded()) r.push_back(p.info);
        return r;
    }();
    return v;
}

std::string selected(const Pack& pack, const Category& category) {
    std::string v;
    if (hostui::get(setting_key(pack, category).c_str(), v))
        for (auto& name : category.presets)
            if (name == v) return v;
    return category.fallback;
}

void select(const Pack& pack, const Category& category, const std::string& preset) {
    hostui::set(setting_key(pack, category).c_str(), preset);
}

bool restart_needed() {
    for (auto& p : packs())
        for (auto& c : p.categories) {
            auto it = active().choices.find(setting_key(p, c));
            if (it != active().choices.end() && it->second != selected(p, c)) return true;
        }
    return false;
}

bool has_shaders() { return !active().shaders.empty(); }

bool has_base(uint64_t base, bool vertex) {
    for (auto& [key, src] : active().shaders)
        if (std::get<0>(key) == base && std::get<2>(key) == vertex) return true;
    return false;
}

std::string shader_source(uint64_t base, uint64_t aux, bool vertex) {
    auto it = active().shaders.find({base, aux, vertex});
    return it == active().shaders.end() ? std::string() : it->second;
}

}  // namespace graphic_packs
