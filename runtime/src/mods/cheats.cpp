// Cheats: see cheats.h. The translated game reads botw_cheats[] (botw_cheats.h) at each cheat's site.
#include "cheats.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../overlay/hostui.h"
#include "../runtime.h"

uint8_t botw_cheats[BOTW_CHEAT_COUNT] = {};  // C linkage from botw_cheats.h

namespace mods::cheats {
namespace {
const Info kInfo[BOTW_CHEAT_COUNT] = {
    {"arrows", "Infinite arrows",
     "Shooting doesn't use up arrows, as long as you carry at least two of the kind you shoot."},
    {"weapons", "Weapons and bows don't break",
     "Swords, spears, clubs, rods and bows lose no durability when you use them. Fire still burns wooden "
     "weapons, and throwing one can still break it."},
    {"shields", "Shields don't break", "Shields lose no durability when they block."},
    {"revali", "Revali's Gale", "Uses aren't counted, so it never needs to recharge."},
    {"daruk", "Daruk's Protection", "Blocked hits aren't counted, so it never needs to recharge."},
    {"urbosa", "Urbosa's Fury", "Uses aren't counted, so it never needs to recharge."},
    {"mipha", "Mipha's Grace", "Recharges as soon as it has revived you."},
    {"hearts", "Infinite hearts", "Damage doesn't take hearts away. Your heart containers stay as they are."},
    {"stamina", "Infinite stamina", "Running, climbing, swimming and gliding don't use up stamina."},
    {"cycleFuel", "Infinite fuel", "Riding the Master Cycle Zero doesn't use up its fuel."},
    {"cycleAnywhere", "Summon it anywhere", "Also in the Gerudo Desert and Eldin, where the game doesn't allow it."},
    {"amiibo", "Read each amiibo more than once a day", "The game otherwise accepts each amiibo once per day."},
};

std::string key_of(int cheat) {
    std::string k = std::string("cheat") + kInfo[cheat].id;
    k[5] = (char)std::toupper((unsigned char)k[5]);
    return k;
}
void store(int cheat, bool on) { __atomic_store_n(&botw_cheats[cheat], (uint8_t)(on ? 1 : 0), __ATOMIC_RELAXED); }
bool valid(int cheat) { return cheat >= 0 && cheat < BOTW_CHEAT_COUNT; }

// WWHD_CHEATS=arrows,shields (or "all"): the run's switches, before the overlay starts
const bool g_env = [] {
    const char* e = getenv("WWHD_CHEATS");
    if (!e) return false;
    std::string list = e;
    for (size_t at = 0; at <= list.size();) {
        size_t end = list.find(',', at);
        if (end == std::string::npos) end = list.size();
        const std::string name = list.substr(at, end - at);
        bool known = name.empty() || name == "none";
        for (int i = 0; i < BOTW_CHEAT_COUNT; i++)
            if (name == "all" || name == kInfo[i].id) store(i, true), known = true;
        if (!known) fprintf(stderr, "[cheats] WWHD_CHEATS: unknown cheat \"%s\"\n", name.c_str());
        at = end + 1;
    }
    return true;
}();
}  // namespace

const Info& info(int cheat) { return kInfo[valid(cheat) ? cheat : 0]; }
bool on(int cheat) { return valid(cheat) && botw_cheat(cheat); }
bool from_env() { return g_env; }

void set(int cheat, bool v) {
    if (!valid(cheat) || on(cheat) == v) return;
    store(cheat, v);
    LOG("[cheats] %s %s", kInfo[cheat].id, v ? "on" : "off");
    if (!g_env) hostui::post([key = key_of(cheat), v] { hostui::set(key.c_str(), v ? "1" : "0"); });
}

void load_saved() {
    if (g_env) {
        for (int i = 0; i < BOTW_CHEAT_COUNT; i++)
            if (on(i)) LOG("[cheats] %s on (WWHD_CHEATS)", kInfo[i].id);
        return;
    }
    std::string v;
    for (int i = 0; i < BOTW_CHEAT_COUNT; i++)
        if (hostui::get(key_of(i).c_str(), v) && v == "1") {
            store(i, true);
            LOG("[cheats] %s on (saved)", kInfo[i].id);
        }
}
}  // namespace mods::cheats
