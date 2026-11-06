// Cheats: see cheats.h. The translated game reads botw_cheats[] (botw_cheats.h) at each cheat's site.
#include "cheats.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../overlay/hostui.h"
#include "../runtime.h"

uint8_t botw_cheats[BOTW_CHEAT_COUNT] = {};  // C linkage from botw_cheats.h

// auto-placement (tools/botw_cheat_patches.py, 029FFB3C): the actor about to be placed is an enemy
extern "C" int botw_cheat_enemy_name(uint32_t name) {
    static const char kPrefix[] = "Enemy_";
    if (!name) return 0;
    for (uint32_t i = 0; kPrefix[i]; i++)
        if (ld8(name + i) != (uint8_t)kPrefix[i]) return 0;
    return 1;
}

namespace mods::cheats {
const char* const kWeatherNames[10] = {"The game's own", "Blue sky", "Cloudy", "Rain", "Heavy rain", "Snow",
                                       "Heavy snow", "Thunderstorm", "Thunderstorm with rain", "Blue sky with rain"};
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
    {"noBloodMoon", "No Blood Moon",
     "Defeated enemies stay defeated: the Blood Moon that brings them back never rises. The game can still "
     "call one when it needs to free memory, and a Blood Moon already due tonight still comes."},
    {"noRandomEnemies", "No random enemy spawns",
     "Enemies the game places around you on its own don't appear: Stal enemies at night, Keese, Chuchus, "
     "Octoroks, Lizalfos and Yiga ambushes. Camps and other enemies placed in the world stay as they are."},
    {"weather", "Weather",
     "Takes over once the current weather change has finished. Places with weather of their own (Divine "
     "Beasts, some quests) keep it.", 9},
};

std::string key_of(int cheat) {
    std::string k = std::string("cheat") + kInfo[cheat].id;
    k[5] = (char)std::toupper((unsigned char)k[5]);
    return k;
}
void store(int cheat, uint8_t v) { __atomic_store_n(&botw_cheats[cheat], v, __ATOMIC_RELAXED); }
bool valid(int cheat) { return cheat >= 0 && cheat < BOTW_CHEAT_COUNT; }
void note(int cheat, const char* why) {
    const uint8_t v = value(cheat);
    if (cheat == BOTW_CHEAT_WEATHER) LOG("[cheats] weather: %s%s", kWeatherNames[v], why);
    else LOG("[cheats] %s %s%s", kInfo[cheat].id, v ? "on" : "off", why);
}

// WWHD_CHEATS=arrows,shields,weather=3 (or "all" switches): the run's choices, before the overlay starts
const bool g_env = [] {
    const char* e = getenv("WWHD_CHEATS");
    if (!e) return false;
    std::string list = e;
    for (size_t at = 0; at <= list.size();) {
        size_t end = list.find(',', at);
        if (end == std::string::npos) end = list.size();
        std::string name = list.substr(at, end - at);
        long v = 1;
        if (size_t eq = name.find('='); eq != std::string::npos) {
            v = strtol(name.c_str() + eq + 1, nullptr, 10);
            name.resize(eq);
        }
        bool known = name.empty() || name == "none";
        for (int i = 0; i < BOTW_CHEAT_COUNT; i++)
            if ((name == "all" && kInfo[i].max == 1) || name == kInfo[i].id) {
                known = true;
                if (v >= 0 && v <= kInfo[i].max) store(i, (uint8_t)v);
            }
        if (!known) fprintf(stderr, "[cheats] WWHD_CHEATS: unknown cheat \"%s\"\n", name.c_str());
        at = end + 1;
    }
    return true;
}();
}  // namespace

const Info& info(int cheat) { return kInfo[valid(cheat) ? cheat : 0]; }
uint8_t value(int cheat) { return valid(cheat) ? (uint8_t)botw_cheat(cheat) : 0; }
bool on(int cheat) { return value(cheat) != 0; }
bool from_env() { return g_env; }

void set(int cheat, bool v) { set_value(cheat, v ? 1 : 0); }
void set_value(int cheat, uint8_t v) {
    if (!valid(cheat) || v > kInfo[cheat].max || value(cheat) == v) return;
    store(cheat, v);
    note(cheat, "");
    if (!g_env) hostui::post([key = key_of(cheat), v] { hostui::set(key.c_str(), std::to_string(v)); });
}

void load_saved() {
    if (g_env) {
        for (int i = 0; i < BOTW_CHEAT_COUNT; i++)
            if (on(i)) note(i, " (WWHD_CHEATS)");
        return;
    }
    std::string s;
    for (int i = 0; i < BOTW_CHEAT_COUNT; i++)
        if (hostui::get(key_of(i).c_str(), s)) {
            const long v = strtol(s.c_str(), nullptr, 10);
            if (v > 0 && v <= kInfo[i].max) {
                store(i, (uint8_t)v);
                note(i, " (saved)");
            }
        }
}
}  // namespace mods::cheats
