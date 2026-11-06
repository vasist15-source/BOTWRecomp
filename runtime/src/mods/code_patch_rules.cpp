// Code mods' presets, programs and pack folders: see code_patch_rules.h.
#include "code_patch_rules.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "../exception_report.h"
#include "mod_hash.h"

namespace mods::patches {
namespace fs = std::filesystem;

namespace {
std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
bool hidden(const std::string& name) {
    return name.empty() || name[0] == '.' || name == "__MACOSX" || name == "Thumbs.db" || name == "desktop.ini";
}
int64_t ival(double v) {  // Python's int() of the translator, for the values programs see
    if (!std::isfinite(v)) return 0;
    if (v >= 9.2e18) return INT64_MAX;
    if (v <= -9.2e18) return INT64_MIN;
    return (int64_t)v;
}
int32_t wrap32(uint64_t v) { return (int32_t)(uint32_t)v; }
double ha(double x) {
    const uint32_t a = to_u32(x);
    return double(((a >> 16) + ((a & 0x8000) ? 1 : 0)) & 0xFFFF);
}
double binary(const std::string& op, double lhs, double rhs, bool integer) {
    if (integer) {
        const int64_t a = ival(lhs), b = ival(rhs);
        if (op == "+") return wrap32((uint64_t)a + (uint64_t)b);
        if (op == "-") return wrap32((uint64_t)a - (uint64_t)b);
        if (op == "*") return wrap32((uint64_t)a * (uint64_t)b);
        if (op == "/") return b == 0 || (a == INT64_MIN && b == -1) ? 0 : wrap32((uint64_t)(a / b));
        if (op == "%") return b == 0 || (a == INT64_MIN && b == -1) ? 0 : double(a % b);
        if (op == "^") {
            const double v = std::pow(double(a), double(b));
            return std::isfinite(v) ? wrap32((uint64_t)ival(v)) : 0;
        }
        if (op == "==") return a == b;
        if (op == "!=") return a != b;
        if (op == "<") return a < b;
        if (op == "<=") return a <= b;
        if (op == ">") return a > b;
        if (op == ">=") return a >= b;
    } else {
        if (op == "+") return lhs + rhs;
        if (op == "-") return lhs - rhs;
        if (op == "*") return lhs * rhs;
        if (op == "/") return rhs == 0.0 ? 0.0 : lhs / rhs;
        if (op == "^") return std::pow(lhs, rhs);
        if (op == "%") {
            const double a = std::round(rhs), b = std::round(lhs);
            if (a == 0 || !std::isfinite(lhs) || !std::isfinite(rhs)) return 0;
            const int64_t ia = ival(a), ib = ival(b);
            if (ia == -1 && ib == INT64_MIN) return 0;
            return double(ib % ia);
        }
        if (op == "==") return std::fabs(lhs - rhs) <= 0.0000000001 ? 1.0 : 0.0;
        if (op == "!=") return std::fabs(lhs - rhs) > 0.0000000001 ? 1.0 : 0.0;
        if (op == "<") return lhs < rhs;
        if (op == "<=") return lhs <= rhs;
        if (op == ">") return lhs > rhs;
        if (op == ">=") return lhs >= rhs;
    }
    exception_report::raise("Unknown operator in a code mod program: " + op);
}
}  // namespace

uint32_t to_u32(double v) {
    if (!std::isfinite(v) || std::fabs(v) >= 9.2e18) return 0;
    return (uint32_t)(uint64_t)(int64_t)v;
}

double evaluate(const json::Value& program, const std::map<std::string, double>& vars, bool integer) {
    if (program.type != json::Value::Array) exception_report::raise("A code mod program is not a list");
    std::vector<double> stack;
    for (const json::Value& t : program.array) {
        if (t.type == json::Value::Number) {
            stack.push_back(integer ? std::trunc(t.number) : t.number);
            continue;
        }
        if (t.type != json::Value::String) exception_report::raise("Malformed code mod program");
        const std::string& s = t.text;
        if (!s.empty() && s[0] == '$') {
            auto it = vars.find(s);
            if (it == vars.end()) exception_report::raise("Unknown preset variable " + s);
            stack.push_back(integer ? std::trunc(it->second) : it->second);
            continue;
        }
        if (s.rfind("fn:", 0) == 0) {
            if (stack.empty()) exception_report::raise("Malformed code mod program");
            const double v = stack.back();
            const std::string fn = s.substr(3);
            if (fn == "ha") stack.back() = ha(v);
            else if (fn == "hi") stack.back() = double((to_u32(v) >> 16) & 0xFFFF);
            else if (fn == "lo") stack.back() = double(to_u32(v) & 0xFFFF);
            else if (fn == "u32") stack.back() = double(to_u32(v));
            else exception_report::raise("Unknown function in a code mod program: " + fn);
            continue;
        }
        if (stack.size() < 2) exception_report::raise("Malformed code mod program");
        const double rhs = stack.back();
        stack.pop_back();
        stack.back() = binary(s, stack.back(), rhs, integer);
    }
    if (stack.empty()) exception_report::raise("Empty code mod program");
    return stack.back();
}

Rules rules_of(const json::Value& mod) {
    Rules r;
    for (const auto& [name, v] : mod.get("variables").object)
        if (v.type == json::Value::Array && v.array.size() == 2) r.defaults[name] = v.array[1].number;
    for (const auto& c : mod.get("categories").array) r.categories.push_back(c.string());
    for (const auto& p : mod.get("presets").array) {
        Preset preset;
        preset.name = p.get("name").string();
        preset.category = p.get("category").string();
        preset.is_default = p.get("default").type == json::Value::Bool && p.get("default").boolean;
        preset.condition = p.get("condition");
        for (const auto& [name, v] : p.get("values").object) preset.values[name] = v.number;
        r.presets.push_back(std::move(preset));
    }
    return r;
}

std::map<std::string, double> values(const Rules& r, const Selection& s) {
    std::map<std::string, double> out;
    for (int pass = 0; pass < 2; pass++)  // active and visible, then active
        for (size_t i = 0; i < r.presets.size(); i++)
            if (s.active[i] && s.visible[i] == (pass == 0))
                for (const auto& [name, v] : r.presets[i].values) out.emplace(name, v);
    for (const auto& [name, v] : r.defaults) out.emplace(name, v);
    return out;
}

Selection select(const Rules& r, const std::map<std::string, std::string>& chosen) {
    const size_t n = r.presets.size();
    Selection s{std::vector<bool>(n, false), std::vector<bool>(n, true)};
    for (const std::string& category : r.categories) {
        long pick = -1, first = -1, marked = -1;
        auto want = chosen.find(category);
        for (size_t i = 0; i < n; i++) {
            if (r.presets[i].category != category) continue;
            if (first < 0) first = long(i);
            if (marked < 0 && r.presets[i].is_default) marked = long(i);
            if (want != chosen.end() && r.presets[i].name == want->second) pick = long(i);
        }
        if (pick < 0) pick = marked >= 0 ? marked : first;
        if (pick >= 0) s.active[size_t(pick)] = true;
    }
    auto update_visibility = [&] {  // in order, each with the values as they are so far (as Cemu)
        for (size_t i = 0; i < n; i++) {
            if (r.presets[i].condition.type != json::Value::Array) {
                s.visible[i] = true;
                continue;
            }
            try {
                s.visible[i] = evaluate(r.presets[i].condition, values(r, s), true) != 0;
            } catch (const std::exception&) {
                s.visible[i] = false;
            }
        }
    };
    update_visibility();
    for (int round = 0; round < 4; round++) {  // ValidatePresetSelections
        bool changed = false;
        for (const std::string& category : r.categories) {
            long fallback = -1, selected = -1, shown = 0;
            for (size_t i = 0; i < n; i++) {
                if (r.presets[i].category != category) continue;
                if (s.visible[i]) {
                    shown++;
                    if (fallback < 0 || r.presets[i].is_default) fallback = long(i);
                }
                if (s.active[i]) selected = long(i);
            }
            if (!shown) continue;
            if (selected < 0) {
                s.active[size_t(fallback)] = true;
                continue;
            }
            if (!s.visible[size_t(selected)]) {
                s.active[size_t(selected)] = false;
                s.active[size_t(fallback)] = true;
                changed = true;
            }
        }
        if (!changed) break;
        update_visibility();
    }
    return s;
}

fs::path pack_folder(const fs::path& mod) {
    fs::path cur = mod;
    for (int depth = 0; depth < 6; depth++) {
        std::error_code ec;
        std::vector<std::string> dirs;
        bool rules = false;
        for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (hidden(name)) continue;
            std::error_code dec;
            if (fs::is_directory(it->path(), dec)) dirs.push_back(name);
            else if (lower(name) == "rules.txt") rules = true;
        }
        if (ec) return {};
        if (rules) return cur;
        std::sort(dirs.begin(), dirs.end());
        for (const auto& d : dirs) {
            const std::string l = lower(d);
            if (l == "content" || l == "aoc" || l == "romfs" || l == "exefs") return {};
        }
        if (dirs.size() != 1) return {};
        cur /= dirs.front();
    }
    return {};
}

std::vector<std::string> pack_files(const fs::path& pack) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(pack, ec), end; !ec && it != end; it.increment(ec)) names.push_back(it->path().filename().string());
    std::sort(names.begin(), names.end());
    std::vector<std::string> out, asm_files, txt;
    for (const auto& n : names)
        if (lower(n) == "rules.txt") {
            out.push_back(n);
            break;
        }
    for (const auto& n : names) {
        const std::string l = lower(n);
        std::error_code fec;
        if (!fs::is_regular_file(pack / n, fec)) continue;
        if (l.size() >= 10 && l.rfind("patch_", 0) == 0 && l.compare(l.size() - 4, 4, ".asm") == 0) asm_files.push_back(n);
        else if (l == "patches.txt") txt.push_back(n);
    }
    out.insert(out.end(), (asm_files.empty() ? txt : asm_files).begin(), (asm_files.empty() ? txt : asm_files).end());
    return out;
}

std::string fingerprint(const fs::path& pack) {
    std::string all;
    for (const std::string& name : pack_files(pack)) {
        std::ifstream f(pack / name, std::ios::binary);
        if (!f) exception_report::raise("Cannot read " + (pack / name).string());
        const std::string data{std::istreambuf_iterator<char>(f), {}};
        all += name;
        all += '\0';
        all += std::to_string(data.size());
        all += '\0';
        all += data;
    }
    return hash::sha256_text(all);
}

}  // namespace mods::patches
