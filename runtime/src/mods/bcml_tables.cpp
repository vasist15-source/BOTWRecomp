// BCML's merge data for game data tables of one file each (actors, map units, shops, drop tables, quests, resident
// actors, area data, status effects, the main field's and the shrines' static data): see bcml_merge.h. Our own
// implementation of the formats and rules of BCML's mergers (NiceneNerd/BCML, GPL-3.0: bcml/mergers actors.py with
// src/mergers/actorinfo.rs, mubin.py with src/mergers/maps.rs, shop.py, drop.py, quests.py, residents.py,
// areadata.py, effects.py, mainstatic.py), with UKMM's uk-manager and uk-content as a second reference. Where
// BCML's code loses data or can't have meant what it does, the notes say what is done instead.
#include "bcml_merge.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "mod_json.h"

namespace mods::bcml {
namespace {

using byml::Type;
using byml::Value;

bool fail(std::string& why, std::string text) {
    why = std::move(text);
    return false;
}
bool parse_doc(const Bytes& b, byml::Document& doc, std::string& why, const std::string& what) {
    std::string bad;
    if (!byml::parse(b.data(), b.size(), doc, &bad)) return fail(why, what + ": " + bad);
    return true;
}
bool write_doc(const byml::Document& doc, Bytes& out, std::string& why, const std::string& what) {
    std::string bad;
    if (!byml::write(doc, out, &bad)) return fail(why, what + ": " + bad);
    return true;
}
bool parse_yaml(const std::string& text, Value& v, std::string& why) {
    std::string bad;
    if (!byml::parse_text(text, v, &bad)) return fail(why, bad);
    if (v.type == Type::Null) v = Value::new_hash();
    return true;
}
// a hash as BCML writes it in a log's keys: decimal (a u32; a negative s32 from older logs as its bits)
bool parse_u32(const std::string& s, uint32_t& out) {
    size_t i = s.size() > 1 && s[0] == '-' ? 1 : 0;
    if (i == s.size() || s.size() - i > 10) return false;
    uint64_t v = 0;
    for (; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + uint64_t(s[i] - '0');
    }
    if (s[0] == '-') {
        if (v > 0x80000000ull) return false;
        out = uint32_t(-int64_t(v));
        return true;
    }
    if (v > 0xFFFFFFFFull) return false;
    out = uint32_t(v);
    return true;
}
bool u32_of(const Value* v, uint32_t& out) {
    int64_t i;
    if (!v || !v->integer(i)) return false;
    out = uint32_t(uint64_t(i));
    return true;
}
void sort_hash(Value& h) {  // a hash built in any order: in key order, as Value keeps them
    std::stable_sort(h.hash.begin(), h.hash.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
}
// a log's table of tables ({key: {...}}): every value a table
bool table_of_tables(const Value& v, std::string& why, const char* what) {
    if (v.type != Type::Hash) return fail(why, std::string("it is not a table of ") + what);
    for (const auto& [key, x] : v.hash)
        if (x.type != Type::Hash) return fail(why, key + " is not a table");
    return true;
}
// the diffs merged as BCML consolidates most logs (util.dict_merge with overwrite_lists: later mods win, key by key)
Value consolidated(const std::vector<const Diff*>& diffs) {
    Value all = Value::new_hash();
    for (const Diff* d : diffs)
        if (d->value.type == Type::Hash) dict_merge(all, d->value, true);
    return all;
}

// ---------------------------------------------------------------- ActorInfo (actors.py, actorinfo.rs)
bool actor_list(Value& root, Value*& actors, std::string& why, const char* what) {
    actors = root.type == Type::Hash ? root.find("Actors") : nullptr;
    if (!actors || actors->type != Type::Array) return fail(why, std::string(what) + " has no Actors list");
    return true;
}
bool read_actors(const std::string& text, Diff& d, std::string& why) {
    if (!parse_yaml(text, d.value, why) || !table_of_tables(d.value, why, "actors")) return false;
    for (const auto& [key, a] : d.value.hash) {
        uint32_t h;
        if (!parse_u32(key, h)) return fail(why, key + " is not an actor's hash");
    }
    return true;
}
// diff_actorinfo: new actors whole, changed ones by the fields that differ
bool actors_of_copy(const Bytes& game, const Bytes& copy, Diff& d, std::string& why) {
    byml::Document g, c;
    Value *ga, *ca;
    if (!parse_doc(game, g, why, "the game's ActorInfo") || !parse_doc(copy, c, why, "its ActorInfo") ||
        !actor_list(g.root, ga, why, "the game's ActorInfo") || !actor_list(c.root, ca, why, "its ActorInfo"))
        return false;
    std::unordered_map<uint32_t, const Value*> stock;
    for (const auto& a : ga->array) stock[aamp::hash(a.str("name"))] = &a;
    d.value = Value::new_hash();
    for (const auto& a : ca->array) {
        const std::string& name = a.str("name");
        if (a.type != Type::Hash || name.empty()) continue;
        const uint32_t h = aamp::hash(name);
        const auto it = stock.find(h);
        if (it == stock.end()) {
            d.value.hash.push_back({std::to_string(h), a});
            continue;
        }
        if (*it->second == a) continue;
        Value fields = Value::new_hash();
        for (const auto& [k, v] : a.hash) {
            const Value* s = it->second->find(k);
            if (!s || *s != v) fields.hash.push_back({k, v});
        }
        if (!fields.hash.empty()) d.value.hash.push_back({std::to_string(h), std::move(fields)});
    }
    sort_hash(d.value);
    return true;
}
bool merge_actors(const Bytes& game, const std::vector<const Diff*>& diffs, Bytes& out, size_t& changed, std::string& why) {
    byml::Document doc;
    Value* actors;
    if (!parse_doc(game, doc, why, "the game's ActorInfo") || !actor_list(doc.root, actors, why, "the game's ActorInfo")) return false;
    // the mods' changes as BCML consolidates them: later mods win field by field; an actor's instSize (the memory the
    // game reserves for it) is the largest any mod gives
    std::map<uint32_t, Value> all;
    std::map<uint32_t, int64_t> inst;
    for (const Diff* d : diffs)
        for (const auto& [key, a] : d->value.hash) {
            uint32_t h;
            if (!parse_u32(key, h) || a.type != Type::Hash) continue;
            int64_t size;
            if (const Value* s = a.find("instSize"); s && s->integer(size) && size > inst[h]) inst[h] = size;
            auto [it, fresh] = all.try_emplace(h, a);
            if (!fresh) dict_merge(it->second, a, true);
        }
    for (const auto& [h, size] : inst) all[h].set("instSize", Value::s32(int32_t(size)));
    // merge_actorinfo: into the game's actors by hash (fields merged, lists replaced), new ones added; the list stays
    // sorted by hash, as the game looks actors up
    std::map<uint32_t, Value> merged;
    for (auto& a : actors->array) merged.emplace(aamp::hash(a.str("name")), std::move(a));
    changed = 0;
    for (const auto& [h, a] : all) {
        auto it = merged.find(h);
        if (it == merged.end()) {
            merged.emplace(h, a);
            changed++;
            continue;
        }
        Value before = it->second;
        if (it->second.type == Type::Hash) dict_merge(it->second, a, true);
        else it->second = a;
        if (it->second != before) changed++;
    }
    if (!changed) {
        why.clear();
        return false;
    }
    Value list = Value::new_array(), hashes = Value::new_array();
    list.array.reserve(merged.size());
    hashes.array.reserve(merged.size());
    for (auto& [h, a] : merged) {
        hashes.array.push_back(h > 0x7FFFFFFFu ? Value::u32(h) : Value::s32(int32_t(h)));
        list.array.push_back(std::move(a));
    }
    *actors = std::move(list);
    doc.root.set("Hashes", std::move(hashes));
    return write_doc(doc, out, why, "ActorInfo");
}

// ---------------------------------------------------------------- map units (mubin.py, maps.rs)
const char* const kSections[] = {"Objs", "Rails"};
bool hash_id(const Value& e, uint32_t& out) { return e.type == Type::Hash && u32_of(e.find("HashId"), out); }
bool check_unit(const std::string& unit, const Value& d, std::string& why) {
    if (d.type != Type::Hash) return fail(why, unit + " is not a table");
    for (const char* s : kSections) {
        const Value* x = d.find(s);
        if (!x) continue;
        if (x->type != Type::Hash) return fail(why, unit + ": " + s + " is not a table");
        const Value* add = x->find("add");
        const Value* mod = x->find("mod");
        const Value* del = x->find("del");
        if (add && add->type != Type::Array) return fail(why, unit + ": " + s + " add is not a list");
        if (mod && mod->type != Type::Hash) return fail(why, unit + ": " + s + " mod is not a table");
        if (del && del->type != Type::Array) return fail(why, unit + ": " + s + " del is not a list");
    }
    return true;
}
// get_map_diff: entries by HashId, new ones added, changed ones whole, missing ones deleted
bool unit_of_copy(const Bytes& game, const Bytes& copy, Diff& d, std::string& why) {
    byml::Document g, c;
    if (!parse_doc(game, g, why, "the game's map unit") || !parse_doc(copy, c, why, "its map unit")) return false;
    d.value = Value::new_hash();
    for (const char* section : kSections) {
        const Value* ge = g.root.find(section);
        const Value* ce = c.root.find(section);
        if (!ge || !ce || ge->type != Type::Array || ce->type != Type::Array) continue;
        std::unordered_map<uint32_t, const Value*> stock;
        for (const auto& e : ge->array) {
            uint32_t h;
            if (hash_id(e, h)) stock.emplace(h, &e);
        }
        std::unordered_set<uint32_t> have;
        Value add = Value::new_array(), mod = Value::new_hash(), del = Value::new_array();
        for (const auto& e : ce->array) {
            uint32_t h;
            if (!hash_id(e, h)) continue;
            have.insert(h);
            const auto it = stock.find(h);
            if (it == stock.end()) add.array.push_back(e);
            else if (*it->second != e) mod.hash.push_back({std::to_string(h), e});
        }
        for (const auto& e : ge->array) {
            uint32_t h;
            if (hash_id(e, h) && !have.count(h)) del.array.push_back(Value::u32(h));
        }
        if (add.array.empty() && mod.hash.empty() && del.array.empty()) continue;
        sort_hash(mod);
        Value s = Value::new_hash();
        s.set("add", std::move(add));
        s.set("del", std::move(del));
        s.set("mod", std::move(mod));
        d.value.set(section, std::move(s));
    }
    return true;
}
bool merge_unit(const Bytes& game, const std::vector<const Diff*>& diffs, Bytes& out, size_t& changed, std::string& why) {
    byml::Document doc;
    if (!parse_doc(game, doc, why, "the game's map unit")) return false;
    if (doc.root.type != Type::Hash) return fail(why, "the game's map unit is not a table");
    changed = 0;
    for (const char* section : kSections) {
        Value* entries = doc.root.find(section);
        if (!entries || entries->type != Type::Array) continue;
        // consolidate_diffs: deletions of any mod; changes by HashId, the highest mod's; new entries by HashId, the
        // highest mod's first
        std::set<uint32_t> del;
        std::map<std::string, const Value*> mod;
        std::vector<const Value*> add;
        std::unordered_set<uint32_t> added;
        for (const Diff* d : diffs) {
            const Value* s = d->value.find(section);
            if (!s || s->type != Type::Hash) continue;
            if (const Value* m = s->find("mod"); m && m->type == Type::Hash)
                for (const auto& [k, e] : m->hash) mod[k] = &e;
            if (const Value* x = s->find("del"); x && x->type == Type::Array)
                for (const auto& h : x->array) {
                    uint32_t v;
                    if (u32_of(&h, v)) del.insert(v);
                }
        }
        for (auto it = diffs.rbegin(); it != diffs.rend(); ++it) {
            const Value* s = (*it)->value.find(section);
            const Value* a = s && s->type == Type::Hash ? s->find("add") : nullptr;
            if (!a || a->type != Type::Array) continue;
            for (const auto& e : a->array) {
                uint32_t h;
                if (hash_id(e, h) && added.insert(h).second) add.push_back(&e);
            }
        }
        if (mod.empty() && del.empty() && add.empty()) continue;
        // merge_entries: changes in place (one for an entry the game lacks is added instead), deletions, then the new
        // entries the game doesn't have; sorted by HashId
        std::unordered_map<uint32_t, size_t> at;
        for (size_t i = 0; i < entries->array.size(); i++) {
            uint32_t h;
            if (!hash_id(entries->array[i], h)) return fail(why, std::string("the game's map unit has an entry without a HashId in ") + section);
            at.emplace(h, i);
        }
        for (const auto& [k, e] : mod) {
            uint32_t h;
            if (!parse_u32(k, h)) continue;
            if (auto i = at.find(h); i != at.end()) {
                if (entries->array[i->second] != *e) {
                    entries->array[i->second] = *e;
                    changed++;
                }
            } else {
                add.push_back(e);
            }
        }
        std::vector<bool> gone(entries->array.size());
        for (uint32_t h : del)
            if (auto i = at.find(h); i != at.end()) {
                gone[i->second] = true;
                changed++;
            }
        Value kept = Value::new_array();
        kept.array.reserve(entries->array.size() + add.size());
        for (size_t i = 0; i < entries->array.size(); i++)
            if (!gone[i]) kept.array.push_back(std::move(entries->array[i]));
        for (const Value* e : add) {
            uint32_t h;
            if (hash_id(*e, h) && !at.count(h)) {
                kept.array.push_back(*e);
                changed++;
            }
        }
        std::stable_sort(kept.array.begin(), kept.array.end(), [](const Value& a, const Value& b) {
            uint32_t x = 0, y = 0;
            hash_id(a, x);
            hash_id(b, y);
            return x < y;
        });
        *entries = std::move(kept);
    }
    if (!changed) {
        why.clear();
        return false;
    }
    return write_doc(doc, out, why, "map unit");
}

// ---------------------------------------------------------------- shops (shop.py)
// A shop (.bshop): Header {TableNum, Table01, ...}; a table {ColumnNum, ItemSort001, ItemName001, ItemNum001,
// ItemAdjustPrice001, ItemLookGetFlg001, ItemAmount001, ...}. BCML's "shop data" form of it is a list with an object
// TableNames (a parameter per table, named and valued by its name) and a list per table (named by its name) of an
// object per item (named by the item) with these parameters by key, without their numbers.
const char* const kShopKeys[] = {"ItemName", "ItemNum", "ItemAdjustPrice", "ItemLookGetFlg", "ItemAmount"};
int item_number(uint32_t name) {  // the NNN of an ItemNameNNN parameter, -1 if it isn't one
    static const std::unordered_map<uint32_t, int> numbers = [] {
        std::unordered_map<uint32_t, int> m;
        char buf[16];
        for (int n = 0; n < 1000; n++) {
            snprintf(buf, sizeof buf, "ItemName%03d", n);
            m.emplace(aamp::hash(buf), n);
        }
        return m;
    }();
    const auto it = numbers.find(name);
    return it == numbers.end() ? -1 : it->second;
}
std::string numbered(const char* key, int n, int digits) {
    char buf[48];
    snprintf(buf, sizeof buf, "%s%0*d", key, digits, n);
    return buf;
}
aamp::Param string64(uint32_t name, const std::string& s) { return {name, aamp::kString64, std::vector<uint8_t>(s.begin(), s.end())}; }
std::vector<uint8_t> le32(uint32_t v) { return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; }  // AAMP values
uint32_t le32(const std::vector<uint8_t>& d) { return uint32_t(d[0]) | uint32_t(d[1]) << 8 | uint32_t(d[2]) << 16 | uint32_t(d[3]) << 24; }
aamp::Param int_param(uint32_t name, int32_t v) { return {name, aamp::kInt, le32(uint32_t(v))}; }
aamp::Param f32_param(uint32_t name, float v) {
    uint32_t u;
    memcpy(&u, &v, 4);
    return {name, aamp::kF32, le32(u)};
}
bool int_of(const aamp::Param* p, int32_t& out) {
    if (!p || p->data.size() < 4 || (p->type != aamp::kInt && p->type != aamp::kU32)) return false;
    out = int32_t(le32(p->data));
    return true;
}
bool float_of(const aamp::Param* p, float& out) {
    if (!p || p->data.size() < 4) return false;
    if (p->type == aamp::kF32) {
        const uint32_t u = le32(p->data);
        memcpy(&out, &u, 4);
        return true;
    }
    int32_t i;
    if (!int_of(p, i)) return false;
    out = float(i);
    return true;
}
// make_shopdata: the Header's tables that have items, their items in the order of their ItemName parameters (an item
// named twice: one entry, at the first's place, with the last one's values)
bool shop_data(const aamp::File& f, aamp::List& out, std::string& why) {
    const aamp::Object* header = aamp::object(f.root, aamp::hash("Header"));
    if (!header) return fail(why, "the shop has no Header");
    out = {};
    aamp::Object names{aamp::hash("TableNames"), {}};
    for (const auto& p : header->params) {
        if (!aamp::is_string(p.type)) continue;
        const std::string table = aamp::text(p);
        const aamp::Object* o = aamp::object(f.root, aamp::hash(table));
        if (!o) return fail(why, "the shop has no table " + table);
        aamp::List items{aamp::hash(table), {}, {}};
        for (const auto& q : o->params) {
            if (!aamp::is_string(q.type)) continue;
            const std::string item = aamp::text(q);
            const int n = item_number(q.name);
            if (n < 0) return fail(why, "shop table " + table + ": " + item + " is not under an ItemName parameter");
            aamp::Object entry{aamp::hash(item), {}};
            for (const char* key : kShopKeys) {
                const aamp::Param* v = aamp::param(*o, aamp::hash(numbered(key, n, 3)));
                if (!v) return fail(why, "shop table " + table + " has no " + numbered(key, n, 3));
                aamp::Param x = *v;
                x.name = aamp::hash(key);
                entry.params.push_back(std::move(x));
            }
            auto same = std::find_if(items.objects.begin(), items.objects.end(), [&](const aamp::Object& x) { return x.name == entry.name; });
            if (same != items.objects.end()) *same = std::move(entry);
            else items.objects.push_back(std::move(entry));
        }
        if (items.objects.empty() || aamp::list(out, items.name)) continue;
        names.params.push_back(string64(aamp::hash(table), table));
        out.lists.push_back(std::move(items));
    }
    out.objects.push_back(std::move(names));
    return true;
}
// subtract_plists: the items of each table of `removals` out of `data`; a table left with none goes
void subtract(aamp::List& data, const aamp::List& removals) {
    for (const auto& rl : removals.lists) {
        auto it = std::find_if(data.lists.begin(), data.lists.end(), [&](const aamp::List& x) { return x.name == rl.name; });
        if (it == data.lists.end()) continue;
        for (const auto& o : rl.objects)
            it->objects.erase(std::remove_if(it->objects.begin(), it->objects.end(), [&](const aamp::Object& x) { return x.name == o.name; }),
                              it->objects.end());
        if (it->lists.empty() && it->objects.empty()) data.lists.erase(it);
    }
}
// get_shop_diff's diff_plist: what `plist` has that `ref` hasn't, or has otherwise (objects by their parameters)
aamp::List diff_plist(const aamp::List& plist, const aamp::List& ref) {
    aamp::List d{plist.name, {}, {}};
    for (const auto& l : plist.lists) {
        const aamp::List* r = aamp::list(ref, l.name);
        if (!r) d.lists.push_back(l);
        else if (!(*r == l)) d.lists.push_back(diff_plist(l, *r));
    }
    for (const auto& o : plist.objects) {
        const aamp::Object* r = aamp::object(ref, o.name);
        if (!r) {
            d.objects.push_back(o);
            continue;
        }
        if (*r == o) continue;
        aamp::Object x{o.name, {}};
        for (const auto& p : o.params) {
            const aamp::Param* q = aamp::param(*r, p.name);
            if (!q || !(*q == p)) x.params.push_back(p);
        }
        d.objects.push_back(std::move(x));
    }
    return d;
}
bool shop_of_copy(const Bytes& game, const Bytes& copy, Diff& d, std::string& why) {
    aamp::File g, c;
    std::string bad;
    if (!aamp::parse(game.data(), game.size(), g, &bad)) return fail(why, "the game's shop: " + bad);
    if (!aamp::parse(copy.data(), copy.size(), c, &bad)) return fail(why, "its shop: " + bad);
    aamp::List gs, cs;
    if (!shop_data(g, gs, why) || !shop_data(c, cs, why)) return false;
    aamp::List adds = diff_plist(cs, gs), rems = diff_plist(gs, cs);
    subtract(rems, adds);
    d.shop = {};
    adds.name = aamp::hash("Additions");
    rems.name = aamp::hash("Removals");
    if (!adds.lists.empty() || !adds.objects.empty()) d.shop.lists.push_back(std::move(adds));
    if (!rems.lists.empty() || !rems.objects.empty()) d.shop.lists.push_back(std::move(rems));
    return true;
}
bool merge_shop(const Bytes& game, const std::vector<const Diff*>& diffs, Bytes& out, size_t& changed, std::string& why) {
    aamp::File f;
    std::string bad;
    if (!aamp::parse(game.data(), game.size(), f, &bad)) return fail(why, "the game's shop: " + bad);
    aamp::List data, original;
    if (!shop_data(f, data, why)) return false;
    original = data;
    // get_all_diffs: the mods' diffs merged (merge_plists: later mods win, parameter by parameter); merge_shopdata:
    // its removals out of the game's shop, its additions in (new items at the end of their table)
    aamp::List all;
    for (const Diff* d : diffs) aamp::merge(all, d->shop);
    if (const aamp::List* r = aamp::list(all, aamp::hash("Removals"))) subtract(data, *r);
    if (const aamp::List* a = aamp::list(all, aamp::hash("Additions"))) aamp::merge(data, *a);
    if (data == original) {
        why.clear();
        return false;
    }
    changed = 0;
    for (const auto& l : data.lists) {  // items that differ from the game's, and the game's that went
        const aamp::List* g = aamp::list(original, l.name);
        for (const auto& o : l.objects) {
            const aamp::Object* x = g ? aamp::object(*g, o.name) : nullptr;
            if (!x || !(*x == o)) changed++;
        }
    }
    for (const auto& g : original.lists) {
        const aamp::List* l = aamp::list(data, g.name);
        for (const auto& o : g.objects)
            if (!l || !aamp::object(*l, o.name)) changed++;
    }
    // make_bshop: the shop written from its shop data
    const aamp::Object* names = aamp::object(data, aamp::hash("TableNames"));
    std::vector<aamp::Object> objects;
    aamp::Object header{aamp::hash("Header"), {}};
    header.params.push_back(int_param(aamp::hash("TableNum"), 0));
    int tables = 0;
    for (const auto& p : names ? names->params : std::vector<aamp::Param>{}) {
        if (!aamp::is_string(p.type)) continue;
        const std::string table = aamp::text(p);
        const aamp::List* items = aamp::list(data, aamp::hash(table));
        if (!items) continue;  // all of its items removed (BCML stops here)
        aamp::Object t{aamp::hash(table), {}};
        t.params.push_back(int_param(aamp::hash("ColumnNum"), 0));
        int n = 0;
        for (const auto& item : items->objects) {
            const bool whole = std::all_of(std::begin(kShopKeys), std::end(kShopKeys), [&](const char* k) { return aamp::param(item, aamp::hash(k)); });
            if (!whole) continue;  // a mod's change to an item this game doesn't sell
            n++;
            t.params.push_back(int_param(aamp::hash(numbered("ItemSort", n, 3)), n - 1));
            for (const char* key : kShopKeys) {
                aamp::Param v = *aamp::param(item, aamp::hash(key));
                v.name = aamp::hash(numbered(key, n, 3));
                t.params.push_back(std::move(v));
            }
        }
        t.params[0] = int_param(aamp::hash("ColumnNum"), n);
        tables++;
        header.params.push_back(string64(aamp::hash(numbered("Table", tables, 2)), table));
        objects.push_back(std::move(t));
    }
    header.params[0] = int_param(aamp::hash("TableNum"), tables);
    objects.insert(objects.begin(), std::move(header));
    f.root.objects = std::move(objects);
    if (!aamp::write(f, out, &bad)) return fail(why, "shop: " + bad);
    return true;
}

// ---------------------------------------------------------------- drop tables (drop.py)
// A drop table (.bdrop): Header {TableNum, Table01, ...}; a table {RepeatNumMin, RepeatNumMax, ApproachType,
// OccurrenceSpeedType, ColumnNum, ItemName01, ItemProbability01, ...}.
const char* const kUnderride = "UNDERRIDE_CONST";
struct GameDrops {
    Drops tables;
    std::vector<const aamp::Object*> objects;  // each table's object in the game's file
};
// _drop_to_dict: the Header's tables that the file has, their items (an item named twice: one entry, at the first's
// place, with the last one's probability)
bool drop_tables(const aamp::File& f, GameDrops& out, std::string& why) {
    const aamp::Object* header = aamp::object(f.root, aamp::hash("Header"));
    if (!header) return fail(why, "the drop table has no Header");
    for (const auto& p : header->params) {
        if (p.name == aamp::hash("TableNum") || !aamp::is_string(p.type)) continue;
        const std::string name = aamp::text(p);
        const aamp::Object* o = aamp::object(f.root, aamp::hash(name));
        if (!o) continue;
        DropTable t;
        if (!int_of(aamp::param(*o, aamp::hash("RepeatNumMin")), t.repeat_min) || !int_of(aamp::param(*o, aamp::hash("RepeatNumMax")), t.repeat_max) ||
            !int_of(aamp::param(*o, aamp::hash("ApproachType")), t.approach) ||
            !int_of(aamp::param(*o, aamp::hash("OccurrenceSpeedType")), t.speed))
            return fail(why, "drop table " + name + " lacks its counts");
        const int n = o->params.size() > 5 ? int((o->params.size() - 5) / 2) : 0;
        for (int i = 1; i <= n; i++) {
            const aamp::Param* item = aamp::param(*o, aamp::hash(numbered("ItemName", i, 2)));
            if (!item || !aamp::is_string(item->type)) continue;
            float prob;
            if (!float_of(aamp::param(*o, aamp::hash(numbered("ItemProbability", i, 2))), prob))
                return fail(why, "drop table " + name + " has no " + numbered("ItemProbability", i, 2));
            const std::string item_name = aamp::text(*item);
            auto same = std::find_if(t.items.begin(), t.items.end(), [&](const auto& x) { return x.first == item_name; });
            if (same != t.items.end()) same->second = prob;
            else t.items.push_back({item_name, prob});
        }
        auto same = std::find_if(out.tables.begin(), out.tables.end(), [&](const auto& x) { return x.first == name; });
        if (same != out.tables.end()) {
            same->second = std::move(t);
            out.objects[size_t(same - out.tables.begin())] = o;
        } else {
            out.tables.push_back({name, std::move(t)});
            out.objects.push_back(o);
        }
    }
    return true;
}
const DropTable* drop_table(const Drops& d, const std::string& name) {
    for (const auto& [n, t] : d)
        if (n == name) return &t;
    return nullptr;
}
bool drops_of_copy(const Bytes& game, const Bytes& copy, Diff& d, std::string& why) {
    aamp::File g, c;
    std::string bad;
    if (!aamp::parse(game.data(), game.size(), g, &bad)) return fail(why, "the game's drop table: " + bad);
    if (!aamp::parse(copy.data(), copy.size(), c, &bad)) return fail(why, "its drop table: " + bad);
    GameDrops gd, cd;
    if (!drop_tables(g, gd, why) || !drop_tables(c, cd, why)) return false;
    // log_drop_file: the mod's file whole, as its log has it (unless it is the game's)
    d.drops = gd.tables == cd.tables ? Drops() : std::move(cd.tables);
    return true;
}
bool merge_drops(const Bytes& game, const std::vector<const Diff*>& diffs, Bytes& out, size_t& changed, std::string& why) {
    aamp::File f;
    std::string bad;
    if (!aamp::parse(game.data(), game.size(), f, &bad)) return fail(why, "the game's drop table: " + bad);
    GameDrops gd;
    if (!drop_tables(f, gd, why)) return false;
    // consolidate_diffs: a later mod's table replaces an earlier one's whole; BCML's UNDERRIDE: the game's probability
    // of the item (UKMM: an item the game's table lacks is left out)
    Drops all;
    for (const Diff* d : diffs)
        for (const auto& [name, t] : d->drops) {
            auto same = std::find_if(all.begin(), all.end(), [&](const auto& x) { return x.first == name; });
            if (same != all.end()) same->second = t;
            else all.push_back({name, t});
        }
    for (auto& [name, t] : all) {
        const DropTable* g = drop_table(gd.tables, name);
        std::vector<std::pair<std::string, std::optional<float>>> items;
        for (auto& [item, prob] : t.items) {
            if (!prob && g)
                for (const auto& [gi, gp] : g->items)
                    if (gi == item) prob = gp;
            if (prob) items.push_back({item, prob});
        }
        t.items = std::move(items);
    }
    // merge_drop_file: the game's tables and items no mod has go; the mods' values; new tables and items at the end
    Drops result;
    for (const auto& [name, g] : gd.tables) {
        const DropTable* m = drop_table(all, name);
        if (!m) continue;
        DropTable r = *m;
        r.items.clear();
        for (const auto& [item, prob] : g.items)
            for (const auto& [mi, mp] : m->items)
                if (mi == item) {
                    r.items.push_back({item, mp});
                    break;
                }
        for (const auto& [mi, mp] : m->items)
            if (std::none_of(r.items.begin(), r.items.end(), [&](const auto& x) { return x.first == mi; })) r.items.push_back({mi, mp});
        result.push_back({name, std::move(r)});
    }
    for (const auto& [name, m] : all)
        if (!drop_table(gd.tables, name)) result.push_back({name, m});
    if (result == gd.tables) {
        why.clear();
        return false;
    }
    changed = 0;
    for (const auto& [name, t] : result) {
        const DropTable* g = drop_table(gd.tables, name);
        if (!g || !(*g == t)) changed++;
    }
    for (const auto& [name, g] : gd.tables)
        if (!drop_table(result, name)) changed++;
    // _dict_to_drop, with each table under its own number (BCML's numbering puts the first table's name under Table00,
    // which the game doesn't read, and the last one's twice); a table as the game has it keeps the game's object
    std::vector<aamp::Object> objects;
    aamp::Object header{aamp::hash("Header"), {int_param(aamp::hash("TableNum"), int32_t(result.size()))}};
    for (size_t i = 0; i < result.size(); i++) header.params.push_back(string64(aamp::hash(numbered("Table", int(i + 1), 2)), result[i].first));
    objects.push_back(std::move(header));
    for (const auto& [name, t] : result) {
        if (const DropTable* g = drop_table(gd.tables, name); g && *g == t) {
            for (size_t k = 0; k < gd.tables.size(); k++)
                if (gd.tables[k].first == name) objects.push_back(*gd.objects[k]);
            continue;
        }
        aamp::Object o{aamp::hash(name), {}};
        o.params.push_back(int_param(aamp::hash("RepeatNumMin"), t.repeat_min));
        o.params.push_back(int_param(aamp::hash("RepeatNumMax"), t.repeat_max));
        o.params.push_back(int_param(aamp::hash("ApproachType"), t.approach));
        o.params.push_back(int_param(aamp::hash("OccurrenceSpeedType"), t.speed));
        o.params.push_back(int_param(aamp::hash("ColumnNum"), int32_t(t.items.size())));
        for (size_t i = 0; i < t.items.size(); i++) {
            o.params.push_back(string64(aamp::hash(numbered("ItemName", int(i + 1), 2)), t.items[i].first));
            o.params.push_back(f32_param(aamp::hash(numbered("ItemProbability", int(i + 1), 2)), *t.items[i].second));
        }
        objects.push_back(std::move(o));
    }
    f.root.objects = std::move(objects);
    if (!aamp::write(f, out, &bad)) return fail(why, "drop table: " + bad);
    return true;
}
bool read_drop_log(const json::Value& v, Diff& d, std::string& why, const std::string& file) {
    if (v.type != json::Value::Object) return fail(why, file + " is not a table of drop tables");
    for (const std::string& name : v.keys) {
        const json::Value& t = v.get(name);
        if (t.type != json::Value::Object) return fail(why, file + ": " + name + " is not a drop table");
        DropTable dt;
        for (const auto& [key, slot] : {std::pair<const char*, int32_t*>{"repeat_num_min", &dt.repeat_min}, {"repeat_num_max", &dt.repeat_max},
                                         {"approach_type", &dt.approach}, {"occurrence_speed_type", &dt.speed}}) {
            const json::Value& n = t.get(key);
            if (n.type != json::Value::Number) return fail(why, file + ": " + name + " has no " + key);
            *slot = int32_t(n.number);
        }
        const json::Value& items = t.get("items");
        if (items.type != json::Value::Object) return fail(why, file + ": " + name + " has no items");
        for (const std::string& item : items.keys) {
            const json::Value& p = items.get(item);
            if (p.type == json::Value::Number) dt.items.push_back({item, float(p.number)});
            else if (p.type == json::Value::String && p.text == kUnderride) dt.items.push_back({item, std::nullopt});
            else return fail(why, file + ": " + name + ": " + item + " has no probability");
        }
        d.drops.push_back({name, std::move(dt)});
    }
    return true;
}

// ---------------------------------------------------------------- quests (quests.py)
const char* const kPrevQuest = "prev_quest";  // a new quest's place: after this one ("--index_zero": first)
bool read_quests(const std::string& text, Diff& d, std::string& why) {
    if (!parse_yaml(text, d.value, why)) return false;
    if (d.value.type != Type::Hash) return fail(why, "it is not a table");
    const Value* add = d.value.find("add");
    const Value* mod = d.value.find("mod");
    const Value* del = d.value.find("del");
    if (add && add->type != Type::Array) return fail(why, "add is not a list");
    if (mod && mod->type != Type::Hash) return fail(why, "mod is not a table");
    if (del && del->type != Type::Array) return fail(why, "del is not a list");
    for (const Value* l : {add, del})
        if (l)
            for (const auto& e : l->array)
                if (l == add ? e.str("Name").empty() : e.type != Type::String) return fail(why, l == add ? "an added quest has no Name" : "a deleted quest is not a name");
    return true;
}
bool quests_of_copy(const Bytes& game, const Bytes& copy, Diff& d, std::string& why) {
    byml::Document g, c;
    if (!parse_doc(game, g, why, "the game's quests") || !parse_doc(copy, c, why, "its quests")) return false;
    if (g.root.type != Type::Array || c.root.type != Type::Array) return fail(why, "the quests are not a list");
    std::unordered_map<std::string, const Value*> stock;
    for (const auto& q : g.root.array) stock.emplace(q.str("Name"), &q);
    std::unordered_set<std::string> have;
    Value add = Value::new_array(), mod = Value::new_hash(), del = Value::new_array();
    for (size_t i = 0; i < c.root.array.size(); i++) {
        const Value& q = c.root.array[i];
        const std::string& name = q.str("Name");
        if (q.type != Type::Hash || name.empty()) continue;
        have.insert(name);
        const auto it = stock.find(name);
        if (it != stock.end() && *it->second == q) continue;
        Value x = q;
        x.set(kPrevQuest, Value::string(i ? c.root.array[i - 1].str("Name") : "--index_zero"));
        if (it == stock.end()) add.array.push_back(std::move(x));
        else mod.set(name, std::move(x));
    }
    for (const auto& q : g.root.array)
        if (!have.count(q.str("Name"))) del.array.push_back(Value::string(q.str("Name")));
    d.value = Value::new_hash();
    d.value.set("add", std::move(add));
    d.value.set("del", std::move(del));
    d.value.set("mod", std::move(mod));
    return true;
}
bool merge_quests(const Bytes& game, const std::vector<const Diff*>& diffs, Bytes& out, size_t& changed, std::string& why) {
    byml::Document doc;
    if (!parse_doc(game, doc, why, "the game's quests")) return false;
    if (doc.root.type != Type::Array) return fail(why, "the game's quests are not a list");
    std::vector<Value>& quests = doc.root.array;
    // consolidate_diffs: new quests by Name, the highest mod's first; changed ones by Name (the highest mod's: BCML takes
    // the lowest one's, against its rule for new ones); deletions of any mod
    std::vector<const Value*> adds;
    std::unordered_set<std::string> named;
    for (auto it = diffs.rbegin(); it != diffs.rend(); ++it)
        if (const Value* a = (*it)->value.find("add"); a && a->type == Type::Array)
            for (const auto& q : a->array)
                if (named.insert(q.str("Name")).second) adds.push_back(&q);
    std::map<std::string, const Value*> mods;
    std::vector<std::string> dels;
    for (const Diff* d : diffs) {
        if (const Value* m = d->value.find("mod"); m && m->type == Type::Hash)
            for (const auto& [name, q] : m->hash) mods[name] = &q;
        if (const Value* x = d->value.find("del"); x && x->type == Type::Array)
            for (const auto& name : x->array)
                if (std::find(dels.begin(), dels.end(), name.text) == dels.end()) dels.push_back(name.text);
    }
    auto index = [&](const std::string& name) -> long {
        for (size_t i = 0; i < quests.size(); i++)
            if (quests[i].str("Name") == name) return long(i);
        return -1;
    };
    changed = 0;
    for (const auto& [name, q] : mods) {
        Value x = *q;
        x.erase(kPrevQuest);
        const long i = index(name);
        if (i < 0) {
            if (named.insert(name).second) adds.push_back(q);
            continue;
        }
        if (quests[size_t(i)] != x) {
            quests[size_t(i)] = std::move(x);
            changed++;
        }
    }
    for (const auto& name : dels)
        for (long i; (i = index(name)) >= 0; changed++) quests.erase(quests.begin() + i);
    for (const Value* q : adds) {
        Value x = *q;
        const std::string prev = x.str(kPrevQuest);
        x.erase(kPrevQuest);
        if (const long i = index(x.str("Name")); i >= 0) {  // a quest the game has after all: changed, not added twice
            if (quests[size_t(i)] != x) {
                quests[size_t(i)] = std::move(x);
                changed++;
            }
            continue;
        }
        long at = long(quests.size());
        if (prev == "--index_zero") at = 0;
        else if (!prev.empty())
            if (const long p = index(prev); p >= 0) at = p + 1;
        quests.insert(quests.begin() + at, std::move(x));
        changed++;
    }
    if (!changed) {
        why.clear();
        return false;
    }
    return write_doc(doc, out, why, "quests");
}

// ---------------------------------------------------------------- resident actors (residents.py)
bool residents_of_copy(const Bytes& game, const Bytes& copy, Diff& d, std::string& why) {
    byml::Document g, c;
    if (!parse_doc(game, g, why, "the game's resident actors") || !parse_doc(copy, c, why, "its resident actors")) return false;
    if (g.root.type != Type::Array || c.root.type != Type::Array) return fail(why, "the resident actors are not a list");
    std::map<std::string, const Value*> stock, mine;
    for (const auto& a : g.root.array) stock[a.str("name")] = &a;
    for (const auto& a : c.root.array) mine[a.str("name")] = &a;
    d.value = Value::new_hash();
    for (const auto& [name, a] : mine)
        if (auto it = stock.find(name); it == stock.end() || *it->second != *a) d.value.hash.push_back({name, *a});
    for (const auto& [name, a] : stock)
        if (!mine.count(name)) {
            Value x = *a;
            x.set("remove", Value::boolean(true));
            d.value.hash.push_back({name, std::move(x)});
        }
    sort_hash(d.value);
    return true;
}
bool merge_residents(const Bytes& game, const std::vector<const Diff*>& diffs, Bytes& out, size_t& changed, std::string& why) {
    byml::Document doc;
    if (!parse_doc(game, doc, why, "the game's resident actors")) return false;
    if (doc.root.type != Type::Array) return fail(why, "the game's resident actors are not a list");
    // merged into the game's by name (fields merged, lists replaced); one with a "remove" key goes. New ones at the end
    // (BCML writes the whole list in name order; the game's order is kept here)
    const Value all = consolidated(diffs);
    std::vector<Value>& list = doc.root.array;
    std::unordered_map<std::string, size_t> at;
    for (size_t i = 0; i < list.size(); i++) at.emplace(list[i].str("name"), i);
    changed = 0;
    std::vector<bool> gone(list.size());
    std::vector<Value> fresh;
    for (const auto& [name, d] : all.hash) {
        if (auto it = at.find(name); it != at.end()) {
            Value& a = list[it->second];
            const Value before = a;
            if (a.type == Type::Hash && d.type == Type::Hash) dict_merge(a, d, true);
            else a = d;
            if (a.find("remove")) gone[it->second] = true;
            if (a.find("remove") || a != before) changed++;
        } else if (d.type == Type::Hash && !d.find("remove")) {
            fresh.push_back(d);
            if (fresh.back().str("name").empty()) fresh.back().set("name", Value::string(name));
            changed++;
        }
    }
    if (!changed) {
        why.clear();
        return false;
    }
    std::vector<Value> kept;
    for (size_t i = 0; i < list.size(); i++)
        if (!gone[i]) kept.push_back(std::move(list[i]));
    for (auto& a : fresh) kept.push_back(std::move(a));
    list = std::move(kept);
    return write_doc(doc, out, why, "resident actors");
}

// ---------------------------------------------------------------- area data (areadata.py)
std::string area_key(const Value& area) {
    int64_t n;
    const Value* v = area.find("AreaNumber");
    return v && v->integer(n) ? std::to_string(n) : std::string();
}
bool areas_of_copy(const Bytes& game, const Bytes& copy, Diff& d, std::string& why) {
    byml::Document g, c;
    if (!parse_doc(game, g, why, "the game's area data") || !parse_doc(copy, c, why, "its area data")) return false;
    if (g.root.type != Type::Array || c.root.type != Type::Array) return fail(why, "the area data is not a list");
    std::unordered_map<std::string, const Value*> stock;
    for (const auto& a : g.root.array) stock.emplace(area_key(a), &a);
    d.value = Value::new_hash();
    for (const auto& a : c.root.array) {
        const std::string key = area_key(a);
        if (key.empty() || a.type != Type::Hash) continue;
        const auto it = stock.find(key);
        if (it == stock.end()) {
            d.value.hash.push_back({key, a});
            continue;
        }
        if (*it->second == a) continue;
        Value fields = Value::new_hash();
        for (const auto& [k, v] : a.hash) {
            const Value* s = it->second->find(k);
            if (!s || *s != v) fields.hash.push_back({k, v});
        }
        if (!fields.hash.empty()) d.value.hash.push_back({key, std::move(fields)});
    }
    sort_hash(d.value);
    return true;
}
bool merge_areas(const Bytes& game, const std::vector<const Diff*>& diffs, Bytes& out, size_t& changed, std::string& why) {
    byml::Document doc;
    if (!parse_doc(game, doc, why, "the game's area data")) return false;
    if (doc.root.type != Type::Array) return fail(why, "the game's area data is not a list");
    const Value all = consolidated(diffs);
    std::vector<Value>& list = doc.root.array;
    std::unordered_map<std::string, size_t> at;
    for (size_t i = 0; i < list.size(); i++) at.emplace(area_key(list[i]), i);
    changed = 0;
    for (const auto& [key, d] : all.hash) {
        if (auto it = at.find(key); it != at.end()) {
            Value& a = list[it->second];
            const Value before = a;
            if (a.type == Type::Hash && d.type == Type::Hash) dict_merge(a, d, true);
            else a = d;
            if (a != before) changed++;
        } else {
            int64_t n = 0;
            const char* s = key.c_str();
            char* end = nullptr;
            n = std::strtoll(s, &end, 10);
            if (key.empty() || *end || d.type != Type::Hash) continue;
            Value a = d;
            if (!a.find("AreaNumber")) a.set("AreaNumber", Value::s32(int32_t(n)));
            list.push_back(std::move(a));
            changed++;
        }
    }
    if (!changed) {
        why.clear();
        return false;
    }
    // written in AreaNumber order, as BCML writes it
    std::stable_sort(list.begin(), list.end(), [](const Value& a, const Value& b) {
        int64_t x = 0, y = 0;
        if (const Value* v = a.find("AreaNumber")) v->integer(x);
        if (const Value* v = b.find("AreaNumber")) v->integer(y);
        return x < y;
    });
    return write_doc(doc, out, why, "area data");
}

// ---------------------------------------------------------------- status effects (effects.py)
Value* effect_table(Value& root) {  // the list's one table of effects
    return root.type == Type::Array && !root.array.empty() && root.array[0].type == Type::Hash ? &root.array[0] : nullptr;
}
bool effects_of_copy(const Bytes& game, const Bytes& copy, Diff& d, std::string& why) {
    byml::Document g, c;
    if (!parse_doc(game, g, why, "the game's status effects") || !parse_doc(copy, c, why, "its status effects")) return false;
    Value* ge = effect_table(g.root);
    Value* ce = effect_table(c.root);
    if (!ge || !ce) return fail(why, "the status effects are not a list of one table");
    d.value = Value::new_hash();
    for (const auto& [name, e] : ce->hash)
        if (const Value* s = ge->find(name); !s || *s != e) d.value.hash.push_back({name, e});
    return true;
}
bool merge_effects(const Bytes& game, const std::vector<const Diff*>& diffs, Bytes& out, size_t& changed, std::string& why) {
    byml::Document doc;
    if (!parse_doc(game, doc, why, "the game's status effects")) return false;
    Value* effects = effect_table(doc.root);
    if (!effects) return fail(why, "the game's status effects are not a list of one table");
    const Value before = *effects;
    dict_merge(*effects, consolidated(diffs), true);
    changed = 0;
    for (const auto& [name, e] : effects->hash)
        if (const Value* b = before.find(name); !b || *b != e) changed++;
    if (!changed) {
        why.clear();
        return false;
    }
    return write_doc(doc, out, why, "status effects");
}

// ---------------------------------------------------------------- the main field's static data (mainstatic.py)
// An entry is known by its position (each coordinate rounded up) and the value of its first key with "name" in it,
// as BCML's get_id: "-1234567-89" + "StartPos_A". Entries without a position can't be told apart and stay as they are.
bool static_id(const Value& item, std::string& out) {
    const Value* t = item.type == Type::Hash ? item.find("Translate") : nullptr;
    if (!t || t->type != Type::Hash) return false;
    out.clear();
    for (const char* axis : {"X", "Y", "Z"}) {
        const Value* v = t->find(axis);
        if (!v) return false;
        double x;
        int64_t i;
        if (v->type == Type::Float) x = v->as_f32();
        else if (v->type == Type::Double) x = v->as_f64();
        else if (v->integer(i)) x = double(i);
        else return false;
        out += std::to_string((long long)std::ceil(x));
    }
    for (const auto& [k, v] : item.hash) {
        std::string lower = k;
        for (char& c : lower) c = char(std::tolower((unsigned char)c));
        if (lower.find("name") == std::string::npos) continue;
        if (v.type != Type::String) return false;
        out += v.text;
        break;
    }
    return true;
}
bool statics_of_copy(const Bytes& game, const Bytes& copy, Diff& d, std::string& why) {
    byml::Document g, c;
    if (!parse_doc(game, g, why, "the game's static data") || !parse_doc(copy, c, why, "its static data")) return false;
    if (g.root.type != Type::Hash || c.root.type != Type::Hash) return fail(why, "the static data is not a table");
    d.value = Value::new_hash();
    for (const auto& [cat, items] : g.root.hash) {
        const Value* mine = c.root.find(cat);
        if (items.type != Type::Array || !mine || mine->type != Type::Array) continue;
        std::map<std::string, const Value*> stock, have;  // by id: the last entry with it, as BCML's dict
        std::string id;
        for (const auto& e : items.array)
            if (static_id(e, id)) stock[id] = &e;
        for (const auto& e : mine->array)
            if (static_id(e, id)) have[id] = &e;
        Value changes = Value::new_hash();
        for (const auto& [k, e] : have)
            if (auto it = stock.find(k); it == stock.end() || *it->second != *e) changes.hash.push_back({k, *e});
        for (const auto& [k, e] : stock)
            if (!have.count(k)) {
                Value x = *e;
                x.set("remove", Value::boolean(true));
                changes.hash.push_back({k, std::move(x)});
            }
        if (changes.hash.empty()) continue;
        sort_hash(changes);
        d.value.hash.push_back({cat, std::move(changes)});
    }
    return true;
}
bool merge_statics(const Bytes& game, const std::vector<const Diff*>& diffs, Bytes& out, size_t& changed, std::string& why) {
    byml::Document doc;
    if (!parse_doc(game, doc, why, "the game's static data")) return false;
    if (doc.root.type != Type::Hash) return fail(why, "the game's static data is not a table");
    const Value all = consolidated(diffs);
    changed = 0;
    for (auto& [cat, items] : doc.root.hash) {
        const Value* d = all.find(cat);
        if (!d || d->type != Type::Hash || items.type != Type::Array) continue;
        // BCML keeps one entry per id (the last); here the others stay as they are: a change goes to the last entry with
        // its id, a removal takes them all
        std::unordered_map<std::string, std::vector<size_t>> at;
        std::string id;
        for (size_t i = 0; i < items.array.size(); i++)
            if (static_id(items.array[i], id)) at[id].push_back(i);
        std::vector<bool> gone(items.array.size());
        std::vector<Value> fresh;
        for (const auto& [k, e] : d->hash) {
            const auto it = at.find(k);
            if (it == at.end()) {
                if (e.type == Type::Hash && !e.find("remove")) {
                    fresh.push_back(e);
                    changed++;
                }
                continue;
            }
            Value& item = items.array[it->second.back()];
            const Value before = item;
            if (item.type == Type::Hash && e.type == Type::Hash) dict_merge(item, e, false);
            else item = e;
            if (item.find("remove")) {
                for (size_t i : it->second) gone[i] = true;
                changed += it->second.size();
            } else if (item != before) {
                changed++;
            }
        }
        std::vector<Value> kept;
        kept.reserve(items.array.size() + fresh.size());
        for (size_t i = 0; i < items.array.size(); i++)
            if (!gone[i]) kept.push_back(std::move(items.array[i]));
        for (auto& e : fresh) kept.push_back(std::move(e));
        items.array = std::move(kept);
    }
    if (!changed) {
        why.clear();
        return false;
    }
    return write_doc(doc, out, why, "static data");
}

// ---------------------------------------------------------------- the shrines' entrances (mubin.py: dungeonstatic)
std::string entrance_id(const Value& e) { return e.str("Map") + "___" + e.str("PosName"); }
bool entrances_of_copy(const Bytes& game, const Bytes& copy, Diff& d, std::string& why) {
    byml::Document g, c;
    if (!parse_doc(game, g, why, "the game's shrine entrances") || !parse_doc(copy, c, why, "its shrine entrances")) return false;
    const Value* gs = g.root.find("StartPos");
    const Value* cs = c.root.find("StartPos");
    if (!gs || !cs || gs->type != Type::Array || cs->type != Type::Array) return fail(why, "the shrine entrances have no StartPos list");
    std::unordered_map<std::string, const Value*> stock;
    for (const auto& e : gs->array) stock.emplace(entrance_id(e), &e);
    d.value = Value::new_hash();
    for (const auto& e : cs->array) {
        const std::string id = entrance_id(e);
        const auto it = stock.find(id);
        if (it == stock.end()) {
            d.value.set(id, e);
            continue;
        }
        Value x = Value::new_hash();
        for (const char* k : {"Rotate", "Translate"}) {
            const Value* a = e.find(k);
            const Value* b = it->second->find(k);
            if (a && (!b || *a != *b)) x.set(k, *a);
        }
        if (!x.hash.empty()) d.value.set(id, std::move(x));
    }
    return true;
}
bool merge_entrances(const Bytes& game, const std::vector<const Diff*>& diffs, Bytes& out, size_t& changed, std::string& why) {
    byml::Document doc;
    if (!parse_doc(game, doc, why, "the game's shrine entrances")) return false;
    Value* list = doc.root.type == Type::Hash ? doc.root.find("StartPos") : nullptr;
    if (!list || list->type != Type::Array) return fail(why, "the game's shrine entrances have no StartPos list");
    // consolidate_diffs: a later mod's entrance replaces an earlier one's whole; merge_dungeonstatic: its keys set in
    // the game's entrance ("Dungeon000" is "Dungeon000___Entrance_1"), a new one added
    std::map<std::string, const Value*> all;
    for (const Diff* d : diffs)
        for (const auto& [k, e] : d->value.hash) all[k] = &e;
    std::unordered_map<std::string, size_t> at;
    for (size_t i = 0; i < list->array.size(); i++) at.emplace(entrance_id(list->array[i]), i);
    changed = 0;
    for (const auto& [k, e] : all) {
        if (e->type != Type::Hash) continue;
        const std::string id = k.find("___") == std::string::npos ? k + "___Entrance_1" : k;
        if (auto it = at.find(id); it != at.end()) {
            Value& entrance = list->array[it->second];
            const Value before = entrance;
            for (const auto& [key, v] : e->hash) entrance.set(key, v);
            if (entrance != before) changed++;
        } else {
            list->array.push_back(*e);
            changed++;
        }
    }
    if (!changed) {
        why.clear();
        return false;
    }
    return write_doc(doc, out, why, "shrine entrances");
}

}  // namespace

// ---------------------------------------------------------------- logs
bool read_log(Kind k, const std::string& text, Diff& d, std::string& why) {
    d = {};
    switch (k) {
    case Kind::ActorInfo: return read_actors(text, d, why);
    case Kind::Quests: return read_quests(text, d, why);
    case Kind::Residents: return parse_yaml(text, d.value, why) && table_of_tables(d.value, why, "resident actors");
    case Kind::AreaData: return parse_yaml(text, d.value, why) && table_of_tables(d.value, why, "areas");
    case Kind::Effects: return parse_yaml(text, d.value, why) && (d.value.type == Type::Hash || fail(why, "it is not a table of status effects"));
    case Kind::MainStatic:
        if (!parse_yaml(text, d.value, why) || !table_of_tables(d.value, why, "categories")) return false;
        for (const auto& [cat, items] : d.value.hash)
            for (const auto& [id, item] : items.hash)
                if (item.type != Type::Hash) return fail(why, cat + ": " + id + " is not a table");
        return true;
    case Kind::DungeonStatic: return parse_yaml(text, d.value, why) && table_of_tables(d.value, why, "shrine entrances");
    case Kind::Map: case Kind::Shop: case Kind::Drop: return fail(why, "a log of several files");
    }
    return false;
}

bool read_maps(const std::string& text, std::map<std::string, Diff>& units, std::string& why) {
    Value v;
    if (!parse_yaml(text, v, why)) return false;
    if (v.type != Type::Hash) return fail(why, "it is not a table of map units");
    // BCML's first logs had only the objects' changes, without Objs/Rails (parse_legacy_diff)
    const bool legacy = !(text.find("Rails") != std::string::npos && text.find("Objs") != std::string::npos);
    for (auto& [unit, d] : v.hash) {
        Diff& out = units[unit];
        if (legacy) {
            out.value = Value::new_hash();
            out.value.set("Objs", std::move(d));
        } else {
            out.value = std::move(d);
        }
        if (!check_unit(unit, out.value, why)) return false;
    }
    return true;
}

bool read_shops(const Bytes& b, std::map<std::string, Diff>& files, std::string& why) {
    aamp::File f;
    std::string bad;
    if (!aamp::parse(b.data(), b.size(), f, &bad)) return fail(why, bad);
    // as deepmerge.aamp: the Filenames object names the files, the list of a file's changes is named by the CRC-32 of
    // its name (BCML numbers the names of merged logs File0, File1, ...: the values count)
    const aamp::Object* names = aamp::object(f.root, aamp::hash("Filenames"));
    if (!names) return fail(why, "it has no Filenames table");
    for (const auto& p : names->params) {
        if (!aamp::is_string(p.type)) continue;
        std::string path = aamp::text(p);
        const aamp::List* l = aamp::list(f.root, aamp::hash(path));
        if (!l) continue;
        std::replace(path.begin(), path.end(), '\\', '/');
        Diff& d = files[path];
        aamp::merge(d.shop, *l);
    }
    return true;
}

bool read_drops(const std::string& text, std::map<std::string, Diff>& files, std::string& why) {
    json::Value v;
    try {
        v = json::parse(text, size_t(256) << 20);
    } catch (const std::exception& e) {
        return fail(why, e.what());
    }
    if (v.type != json::Value::Object) return fail(why, "it is not a table of drop table files");
    for (const std::string& file : v.keys) {
        std::string path = file;
        std::replace(path.begin(), path.end(), '\\', '/');
        Diff& d = files[path];
        d = {};
        if (!read_drop_log(v.get(file), d, why, file)) return false;
    }
    return true;
}

size_t entries(Kind k, const Diff& d) {
    size_t n = 0;
    switch (k) {
    case Kind::Map:
        for (const char* s : kSections)
            if (const Value* x = d.value.find(s))
                for (const char* part : {"add", "mod", "del"})
                    if (const Value* p = x->find(part)) n += p->type == Type::Hash ? p->hash.size() : p->array.size();
        return n;
    case Kind::Shop:
        for (const auto& part : d.shop.lists)
            for (const auto& table : part.lists) n += table.objects.size();
        return n;
    case Kind::Drop: return d.drops.size();
    case Kind::Quests:
        for (const char* part : {"add", "mod", "del"})
            if (const Value* p = d.value.find(part)) n += p->type == Type::Hash ? p->hash.size() : p->array.size();
        return n;
    case Kind::MainStatic:
        for (const auto& [cat, items] : d.value.hash) n += items.hash.size();
        return n;
    default: return d.value.hash.size();
    }
}

bool diff_of_copy(Kind k, const Bytes& game, const Bytes& copy, Diff& d, std::string& why) {
    d = {};
    switch (k) {
    case Kind::ActorInfo: return actors_of_copy(game, copy, d, why);
    case Kind::Map: return unit_of_copy(game, copy, d, why);
    case Kind::Shop: return shop_of_copy(game, copy, d, why);
    case Kind::Drop: return drops_of_copy(game, copy, d, why);
    case Kind::Quests: return quests_of_copy(game, copy, d, why);
    case Kind::Residents: return residents_of_copy(game, copy, d, why);
    case Kind::AreaData: return areas_of_copy(game, copy, d, why);
    case Kind::Effects: return effects_of_copy(game, copy, d, why);
    case Kind::MainStatic: return statics_of_copy(game, copy, d, why);
    case Kind::DungeonStatic: return entrances_of_copy(game, copy, d, why);
    }
    return false;
}

bool merge_file(Kind k, const Bytes& game, const std::vector<const Diff*>& diffs, Bytes& out, size_t& changed, std::string& why) {
    changed = 0;
    switch (k) {
    case Kind::ActorInfo: return merge_actors(game, diffs, out, changed, why);
    case Kind::Map: return merge_unit(game, diffs, out, changed, why);
    case Kind::Shop: return merge_shop(game, diffs, out, changed, why);
    case Kind::Drop: return merge_drops(game, diffs, out, changed, why);
    case Kind::Quests: return merge_quests(game, diffs, out, changed, why);
    case Kind::Residents: return merge_residents(game, diffs, out, changed, why);
    case Kind::AreaData: return merge_areas(game, diffs, out, changed, why);
    case Kind::Effects: return merge_effects(game, diffs, out, changed, why);
    case Kind::MainStatic: return merge_statics(game, diffs, out, changed, why);
    case Kind::DungeonStatic: return merge_entrances(game, diffs, out, changed, why);
    }
    return false;
}

}  // namespace mods::bcml
