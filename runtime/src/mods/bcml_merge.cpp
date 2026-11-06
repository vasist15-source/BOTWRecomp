// BCML's merge data for game data tables: see bcml_merge.h.
#include "bcml_merge.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "botw_formats.h"
#include "mod_json.h"
#include "msbt.h"

namespace mods::bcml {
namespace {

using byml::Type;
using byml::Value;

bool fail(std::string& why, std::string text) {
    why = std::move(text);
    return false;
}
bool parse_doc(const Bytes& b, byml::Document& doc, std::string& why, const std::string& what) {
    Bytes unpacked;
    const Bytes* data = &b;
    if (formats::is_yaz0(b.data(), b.size())) {
        if (!formats::yaz0_decode(b.data(), b.size(), unpacked)) return fail(why, what + ": bad Yaz0 data");
        data = &unpacked;
    }
    std::string bad;
    if (!byml::parse(data->data(), data->size(), doc, &bad)) return fail(why, what + ": " + bad);
    return true;
}
bool write_doc(const byml::Document& doc, Bytes& out, std::string& why, const std::string& what) {
    std::string bad;
    if (!byml::write(doc, out, &bad)) return fail(why, what + ": " + bad);
    return true;
}
// "/revival_bool_data_4.bgdata" -> "/revival_bool_data", 4
bool split_name(const std::string& name, std::string& prefix, int& index) {
    const size_t dot = name.rfind('.');
    const size_t us = name.rfind('_', dot);
    if (dot == std::string::npos || us == std::string::npos || us + 1 >= dot) return false;
    char* end = nullptr;
    const long v = std::strtol(name.c_str() + us + 1, &end, 10);
    if (end != name.c_str() + dot || v < 0) return false;
    prefix = name.substr(0, us);
    index = int(v);
    return true;
}
const std::string& data_name(const Value& flag) { return flag.str("DataName"); }

// ---------------------------------------------------------------- gamedata files
struct FlagFile {
    std::string name, prefix, type;
    int index = 0;
    byml::Document doc;  // parsed when the merge touches it
    bool parsed = false, dirty = false;
    size_t count = 0;  // flags in it
    Value* flags() { return doc.root.find(type); }
};
bool open_flags(const std::string& name, const Bytes& data, FlagFile& f, std::string& why) {
    if (!parse_doc(data, f.doc, why, name)) return false;
    if (f.doc.root.type != Type::Hash || f.doc.root.hash.size() != 1 || f.doc.root.hash[0].second.type != Type::Array)
        return fail(why, name + " is not a flag list");
    f.type = f.doc.root.hash[0].first;
    f.count = f.doc.root.hash[0].second.array.size();
    f.parsed = true;
    return true;
}
// the stages whose object flags the game keeps with the revival ones (UKMM's GameDataPack::STAGES)
bool stage_flag(const std::string& name, bool bools) {
    const std::string first = name.substr(0, name.find('_'));
    const bool stage = first == "MainField" || first == "AocField" || first == "CDungeon" || first == "MainFieldDungeon";
    return stage && !(bools && name.find("HiddenKorok") != std::string::npos);
}
constexpr size_t kFlagsPerFile = 4096, kSaveFlagsPerFile = 8192;

// ---------------------------------------------------------------- save data files
bool save_file_name(const Value& root, std::string& out) {
    const Value* list = root.find("file_list");
    if (!list || list->type != Type::Array || list->array.size() != 2 || list->array[0].type != Type::Hash) return false;
    out = list->array[0].str("file_name");
    return true;
}
bool save_hash(const Value& e, int32_t& out) {
    const Value* h = e.find("HashValue");
    int64_t v;
    if (!h || !h->integer(v)) return false;
    out = int32_t(uint32_t(uint64_t(v)));
    return true;
}
int file_index(const std::string& name) {
    std::string prefix;
    int index = -1;
    return split_name(name, prefix, index) ? index : -1;
}
std::vector<std::string> by_index(const Files& files) {  // file names in their number's order
    std::vector<std::string> names;
    for (const auto& [name, data] : files) names.push_back(name);
    std::stable_sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return file_index(a) < file_index(b); });
    return names;
}

// ---------------------------------------------------------------- texts
bool open_msbt(const std::string& name, const Bytes& data, msbt::File& f, std::string& why) {
    std::string bad;
    if (!msbt::parse(data.data(), data.size(), f, &bad)) return fail(why, name + ": " + bad);
    if (!f.big_endian) return fail(why, name + " is little-endian (Switch)");
    return true;
}

}  // namespace

void dict_merge(Value& dst, const Value& src, bool overwrite_lists) {
    for (const auto& [key, v] : src.hash) {
        Value* d = dst.find(key);
        if (d && d->type == Type::Hash && v.type == Type::Hash) dict_merge(*d, v, overwrite_lists);
        else if (d && d->type == Type::Array && v.type == Type::Array) {
            if (overwrite_lists) *d = v;
            else d->array.insert(d->array.end(), v.array.begin(), v.array.end());
        } else {
            dst.set(key, v);
        }
    }
}

// ---------------------------------------------------------------- EventInfo
bool read_events(const std::string& yaml, Value& diff, std::string& why) {
    std::string bad;
    if (!byml::parse_text(yaml, diff, &bad)) return fail(why, bad);
    if (diff.type == Type::Null) diff = Value::new_hash();
    if (diff.type != Type::Hash) return fail(why, "it is not a table of events");
    return true;
}
bool events_of_copy(const Bytes& game, const Bytes& copy, Value& diff, std::string& why) {
    byml::Document g, c;
    if (!parse_doc(game, g, why, "the game's EventInfo") || !parse_doc(copy, c, why, "its EventInfo")) return false;
    if (g.root.type != Type::Hash || c.root.type != Type::Hash) return fail(why, "EventInfo is not a table of events");
    diff = Value::new_hash();
    for (const auto& [event, data] : c.root.hash) {
        const Value* v = g.root.find(event);
        if (!v || *v != data) diff.hash.push_back({event, data});  // c.root.hash is sorted: so is diff
    }
    return true;
}
bool merge_events(const Bytes& game, const std::vector<const Value*>& diffs, Bytes& out, size_t& changed, std::string& why) {
    byml::Document doc;
    if (!parse_doc(game, doc, why, "the game's EventInfo")) return false;
    if (doc.root.type != Type::Hash) return fail(why, "the game's EventInfo is not a table of events");
    const Value original = doc.root;
    for (const Value* d : diffs)
        for (const auto& [event, data] : d->hash) doc.root.set(event, data);  // whole entries (BCML: shallow)
    changed = 0;
    for (const auto& [event, data] : doc.root.hash) {
        const Value* v = original.find(event);
        if (!v || *v != data) changed++;
    }
    return write_doc(doc, out, why, "EventInfo");
}

// ---------------------------------------------------------------- gamedata
bool read_flags(const std::string& yaml, Value& diff, std::string& why) {
    std::string bad;
    if (!byml::parse_text(yaml, diff, &bad)) return fail(why, bad);
    if (diff.type == Type::Null) diff = Value::new_hash();
    if (diff.type != Type::Hash) return fail(why, "it is not a table of flag types");
    for (const auto& [type, d] : diff.hash) {
        if (d.type != Type::Hash) return fail(why, type + " is not a table");
        const Value* add = d.find("add");
        const Value* del = d.find("del");
        if (add && add->type != Type::Hash && add->type != Type::Null) return fail(why, type + ": add is not a table");
        if (del && del->type != Type::Array && del->type != Type::Null) return fail(why, type + ": del is not a list");
    }
    return true;
}

bool flags_of_copy(const Files& game, const Files& copy, Value& diff, std::string& why) {
    std::map<std::string, std::map<std::string, Value>> g, c;  // type -> DataName -> flag
    for (const auto& [files, out] : {std::pair<const Files*, decltype(&g)>{&game, &g}, {&copy, &c}})
        for (const auto& [name, data] : *files) {
            FlagFile f;
            if (!open_flags(name, data, f, why)) return false;
            for (auto& flag : f.flags()->array) (*out)[f.type][data_name(flag)] = std::move(flag);
        }
    // deletions only from a whole copy (a partial one leaves files out, not flags)
    const bool whole = std::all_of(game.begin(), game.end(), [&](const auto& e) { return copy.count(e.first) > 0; });
    diff = Value::new_hash();
    for (const auto& [type, flags] : c) {
        Value d = Value::new_hash();
        Value add = Value::new_hash(), del = Value::new_array();
        const auto gt = g.find(type);
        for (const auto& [name, flag] : flags) {
            const Value* old = nullptr;
            if (gt != g.end())
                if (auto it = gt->second.find(name); it != gt->second.end()) old = &it->second;
            if (!old || *old != flag) add.set(name, flag);
        }
        if (whole && gt != g.end())
            for (const auto& [name, flag] : gt->second)
                if (!flags.count(name)) del.array.push_back(Value::string(name));
        if (add.hash.empty() && del.array.empty()) continue;
        d.set("add", std::move(add));
        d.set("del", std::move(del));
        diff.set(type, std::move(d));
    }
    return true;
}

bool merge_flags(const Files& game, const std::vector<const Value*>& diffs, Files& changed, size_t& count, std::string& why) {
    Value all = Value::new_hash();
    for (const Value* d : diffs) dict_merge(all, *d, true);  // BCML: a later mod's del list replaces an earlier one's
    std::unordered_set<std::string> touched;
    for (const auto& [type, d] : all.hash) {
        if (const Value* add = d.find("add"); add && add->type == Type::Hash)
            for (const auto& [name, flag] : add->hash) touched.insert(name);
        if (const Value* del = d.find("del"); del && del->type == Type::Array)
            for (const auto& name : del->array) touched.insert(name.text);
    }
    // the game's files: parsed when the merge changes them; where each touched flag is
    std::vector<FlagFile> files;
    std::map<std::string, std::map<std::string, std::pair<size_t, size_t>>> located;  // type -> DataName -> (file, position)
    for (const auto& [name, data] : game) {
        FlagFile f;
        f.name = name;
        if (!split_name(name, f.prefix, f.index)) continue;  // not a flag file
        if (!open_flags(name, data, f, why)) return false;
        bool keep = false;
        const Value& list = *f.flags();
        for (size_t k = 0; k < list.array.size(); k++) {
            const std::string& n = data_name(list.array[k]);
            if (touched.count(n)) {
                located[f.type][n] = {files.size(), k};
                keep = true;
            }
        }
        if (!keep) {  // only its size is needed (it may take new flags: parsed again then)
            f.doc = {};
            f.parsed = false;
        }
        files.push_back(std::move(f));
    }
    auto parsed = [&](FlagFile& f) -> bool { return f.parsed || open_flags(f.name, game.at(f.name), f, why); };
    std::set<std::pair<size_t, size_t>> removed;
    count = 0;
    for (const auto& [type, d] : all.hash) {
        const bool known = std::any_of(files.begin(), files.end(), [&](const FlagFile& f) { return f.type == type; });
        if (!known) continue;  // BCML merges only the game's flag types
        auto& names = located[type];
        std::vector<std::pair<std::string, const Value*>> fresh;
        if (const Value* add = d.find("add"); add && add->type == Type::Hash)
            for (const auto& [name, flag] : add->hash) {
                if (flag.type != Type::Hash) return fail(why, type + ": " + name + " is not a flag");
                Value set = flag;
                if (data_name(set).empty()) set.set("DataName", Value::string(name));
                if (auto it = names.find(name); it != names.end()) {
                    FlagFile& f = files[it->second.first];
                    Value& slot = f.flags()->array[it->second.second];
                    if (slot != set) {
                        slot = std::move(set);
                        f.dirty = true;
                        count++;
                    }
                } else {
                    fresh.push_back({name, &flag});
                }
            }
        std::set<std::string> dels;
        if (const Value* del = d.find("del"); del && del->type == Type::Array)
            for (const auto& name : del->array) dels.insert(name.text);
        for (const auto& name : dels)
            if (auto it = names.find(name); it != names.end() && removed.insert(it->second).second) {
                files[it->second.first].dirty = true;
                count++;
            }
        // new flags go at the end of the last file of their kind, a new file when it is full
        for (const auto& [name, flag] : fresh) {
            if (dels.count(name)) continue;
            auto has = [&](const std::string& p) {
                return std::any_of(files.begin(), files.end(), [&](const FlagFile& f) { return f.prefix == p && f.type == type; });
            };
            std::string prefix;  // the files it joins: "/revival_bool_data", "/bool_data", "/string32_data", ...
            if (type == "bool_data" || type == "s32_data") {
                const std::string want = (stage_flag(name, type == "bool_data") ? "/revival_" : "/") + type;
                if (has(want)) prefix = want;
            }
            if (prefix.empty() && has("/" + type)) prefix = "/" + type;
            if (prefix.empty())
                for (const FlagFile& f : files)
                    if (f.type == type && (prefix.empty() || f.prefix < prefix)) prefix = f.prefix;
            size_t last = files.size();  // the highest number of that prefix
            for (size_t k = 0; k < files.size(); k++)
                if (files[k].type == type && files[k].prefix == prefix && (last == files.size() || files[k].index > files[last].index)) last = k;
            if (last == files.size()) continue;
            if (files[last].count >= kFlagsPerFile) {
                FlagFile f;
                f.prefix = prefix;
                f.index = files[last].index + 1;
                f.name = prefix + "_" + std::to_string(f.index) + ".bgdata";
                f.type = type;
                if (!parsed(files[last])) return false;
                f.doc.version = files[last].doc.version;
                f.doc.big_endian = files[last].doc.big_endian;
                f.doc.root = Value::new_hash();
                f.doc.root.set(type, Value::new_array());
                f.parsed = true;
                files.push_back(std::move(f));
                last = files.size() - 1;
            }
            FlagFile& f = files[last];
            if (!parsed(f)) return false;
            Value set = *flag;
            if (data_name(set).empty()) set.set("DataName", Value::string(name));
            f.flags()->array.push_back(std::move(set));
            f.count++;
            f.dirty = true;
            count++;
        }
    }
    for (size_t i = 0; i < files.size(); i++) {
        FlagFile& f = files[i];
        if (!f.dirty) continue;
        Value& list = *f.flags();
        Value kept = Value::new_array();
        kept.array.reserve(list.array.size());
        for (size_t k = 0; k < list.array.size(); k++)
            if (!removed.count({i, k})) kept.array.push_back(std::move(list.array[k]));
        list = std::move(kept);
        Bytes out;
        if (!write_doc(f.doc, out, why, f.name)) return false;
        changed[f.name] = std::move(out);
    }
    return true;
}

// ---------------------------------------------------------------- savedata
bool read_save(const std::string& yaml, SaveDiff& diff, std::string& why) {
    Value v;
    std::string bad;
    if (!byml::parse_text(yaml, v, &bad)) return fail(why, bad);
    if (v.type == Type::Null) return true;
    if (v.type != Type::Hash) return fail(why, "it is not a table");
    if (const Value* add = v.find("add"); add && add->type == Type::Array)
        for (const auto& e : add->array) {
            int32_t h;
            if (e.type != Type::Hash || !save_hash(e, h)) return fail(why, "an added flag has no HashValue");
            diff.add.push_back(e);
        }
    if (const Value* del = v.find("del"); del && del->type == Type::Array)
        for (const auto& e : del->array) {
            int64_t h;
            if (!e.integer(h)) return fail(why, "a deleted flag is not a hash");
            diff.del.push_back(int32_t(uint32_t(uint64_t(h))));
        }
    return true;
}

namespace {
// the game_data.sav flags of a save data archive, and its first such file (the layout of the others)
bool save_flags(const Files& files, std::vector<Value>& flags, byml::Document* first, std::vector<std::string>* others, std::string& why) {
    bool have = false;
    for (const std::string& name : by_index(files)) {
        byml::Document doc;
        if (!parse_doc(files.at(name), doc, why, name)) return false;
        std::string sav;
        if (doc.root.type != Type::Hash || !save_file_name(doc.root, sav)) return fail(why, name + " is not a save data format");
        if (sav != "game_data.sav") {
            if (others) others->push_back(name);
            continue;
        }
        Value& list = doc.root.find("file_list")->array[1];
        if (list.type != Type::Array) return fail(why, name + " has no flag list");
        for (auto& e : list.array) flags.push_back(std::move(e));
        if (!have && first) {
            list.array.clear();
            *first = std::move(doc);
        }
        have = true;
    }
    if (!have) return fail(why, "no game_data.sav format");
    return true;
}
}  // namespace

bool save_of_copy(const Files& game, const Files& copy, SaveDiff& diff, std::string& why) {
    std::vector<Value> g, c;
    if (!save_flags(game, g, nullptr, nullptr, why) || !save_flags(copy, c, nullptr, nullptr, why)) return false;
    std::unordered_set<int32_t> gh, ch;
    for (const auto& e : g) {
        int32_t h;
        if (save_hash(e, h)) gh.insert(h);
    }
    for (const auto& e : c) {
        int32_t h;
        if (!save_hash(e, h)) continue;
        ch.insert(h);
        if (!gh.count(h)) diff.add.push_back(e);
    }
    const bool whole = std::all_of(game.begin(), game.end(), [&](const auto& e) { return copy.count(e.first) > 0; });
    if (whole)
        for (int32_t h : gh)
            if (!ch.count(h)) diff.del.push_back(h);
    std::sort(diff.del.begin(), diff.del.end());
    return true;
}

bool merge_save(const Files& game, const std::vector<const SaveDiff*>& diffs, Files& result, size_t& count, std::string& why) {
    why.clear();
    // BCML: the highest mod's entry for a hash wins; deletions of any mod apply
    std::vector<const Value*> adds;
    std::unordered_set<int32_t> seen;
    std::set<int32_t> dels;
    for (auto it = diffs.rbegin(); it != diffs.rend(); ++it) {
        for (const auto& e : (*it)->add) {
            int32_t h;
            if (save_hash(e, h) && seen.insert(h).second) adds.push_back(&e);
        }
        dels.insert((*it)->del.begin(), (*it)->del.end());
    }
    std::vector<Value> flags;
    byml::Document first;
    std::vector<std::string> others;
    if (!save_flags(game, flags, &first, &others, why)) return false;
    std::map<int32_t, Value> merged;  // by hash, as a signed number: the game's order
    for (auto& e : flags) {
        int32_t h;
        if (save_hash(e, h)) merged[h] = e;
    }
    count = 0;
    for (const Value* e : adds) {
        int32_t h;
        save_hash(*e, h);
        auto it = merged.find(h);
        if (it == merged.end() || it->second != *e) count++;
        merged[h] = *e;
    }
    for (int32_t h : dels) count += merged.erase(h);
    // unchanged: the game's files
    bool same = merged.size() == flags.size();
    if (same) {
        size_t k = 0;
        for (const auto& [h, e] : merged)
            if (e != flags[k++]) {
                same = false;
                break;
            }
    }
    if (same) return false;
    result.clear();
    std::vector<Value> sorted;
    sorted.reserve(merged.size());
    for (auto& [h, e] : merged) sorted.push_back(std::move(e));
    size_t files = 0;
    for (size_t at = 0; at < sorted.size() || files == 0; at += kSaveFlagsPerFile, files++) {
        byml::Document doc = first;
        Value& list = doc.root.find("file_list")->array[1];
        const size_t end = std::min(sorted.size(), at + kSaveFlagsPerFile);
        list.array.assign(sorted.begin() + long(at), sorted.begin() + long(end));
        Bytes out;
        const std::string name = "/saveformat_" + std::to_string(files) + ".bgsvdata";
        if (!write_doc(doc, out, why, name)) return false;
        result[name] = std::move(out);
        if (end >= sorted.size()) {
            files++;
            break;
        }
    }
    for (const std::string& name : others) result["/saveformat_" + std::to_string(files++) + ".bgsvdata"] = game.at(name);
    return true;
}

// ---------------------------------------------------------------- texts
bool read_texts(const std::string& text, std::map<std::string, TextFiles>& diff, std::string& why) {
    json::Value v;
    try {
        v = json::parse(text, size_t(512) << 20);
    } catch (const std::exception& e) {
        return fail(why, e.what());
    }
    if (v.type != json::Value::Object) return fail(why, "it is not a table of languages");
    for (const auto& [language, files] : v.object) {
        if (files.type != json::Value::Object) return fail(why, language + " is not a table of files");
        TextFiles& out = diff[language];
        for (const auto& [file, entries] : files.object) {
            if (entries.type != json::Value::Object) return fail(why, file + " is not a table of texts");
            std::string name = file;
            for (size_t at = name.find("msyt"); at != std::string::npos; at = name.find("msyt", at + 4)) name.replace(at, 4, "msbt");
            auto& texts = out[name];
            for (const auto& [label, entry] : entries.object) {
                Text t;
                std::string bad;
                if (!msbt::encode(entry.get("contents"), true, t.text, &bad)) return fail(why, file + " " + label + ": " + bad);
                const json::Value& a = entry.get("attributes");
                if (a.type == json::Value::String) {
                    t.has_attribute = true;
                    t.attribute = msbt::encode_string(a.text, true, true);
                }
                texts[label] = std::move(t);
            }
        }
    }
    return true;
}

bool texts_of_copy(const Files& game, const Files& copy, TextFiles& diff, std::string& why) {
    for (const auto& [name, data] : copy) {
        if (name.size() < 5 || name.compare(name.size() - 5, 5, ".msbt")) continue;
        msbt::File c, g;
        if (!open_msbt(name, data, c, why)) return false;
        const auto it = game.find(name);
        const bool known = it != game.end() && open_msbt(name, it->second, g, why);
        if (it != game.end() && !known) return false;
        for (const auto& e : c.entries) {
            const msbt::Entry* old = known ? msbt::find(g, e.label) : nullptr;
            if (old && old->text == e.text && (!c.atr_strings || old->attribute == e.attribute)) continue;
            diff[name][e.label] = {e.text, c.atr_strings, e.attribute};
        }
    }
    return true;
}

const TextFiles* texts_for(const std::map<std::string, TextFiles>& diff, const std::string& language) {
    if (auto it = diff.find(language); it != diff.end()) return &it->second;
    auto code = [](const std::string& l) { return l.size() >= 4 ? l.substr(2, 2) : l; };
    for (const auto& [l, t] : diff)
        if (code(l) == code(language)) return &t;
    for (const auto& [l, t] : diff)
        if (code(l) == "en") return &t;
    return diff.empty() ? nullptr : &diff.begin()->second;
}

bool merge_texts(const Files& game, const std::vector<const TextFiles*>& diffs, Files& changed, size_t& count, std::string& why) {
    TextFiles all;
    for (const TextFiles* d : diffs)
        for (const auto& [file, texts] : *d)
            for (const auto& [label, t] : texts) all[file][label] = t;
    count = 0;
    for (const auto& [file, texts] : all) {
        msbt::File f;
        const auto it = game.find(file);
        const bool fresh = it == game.end();
        if (fresh) {
            f = msbt::make(file, true);
        } else if (!open_msbt(file, it->second, f, why)) {
            return false;
        }
        size_t here = 0;
        for (const auto& [label, t] : texts) {
            const msbt::Entry* old = msbt::find(f, label);
            Bytes attribute = t.has_attribute ? t.attribute : old ? old->attribute : Bytes();
            if (old && old->text == t.text && (!f.atr_strings || old->attribute == attribute)) continue;
            msbt::set(f, label, t.text, std::move(attribute));
            here++;
        }
        if (!here) continue;
        count += here;
        if (fresh) {  // BCML: one bucket per text
            f.groups = uint32_t(std::max<size_t>(1, f.entries.size()));
            for (auto& e : f.entries) e.bucket = msbt::label_hash(e.label, f.groups);
        }
        Bytes out;
        std::string bad;
        if (!msbt::write(f, out, &bad)) return fail(why, file + ": " + bad);
        changed[file] = std::move(out);
    }
    return true;
}

}  // namespace mods::bcml
