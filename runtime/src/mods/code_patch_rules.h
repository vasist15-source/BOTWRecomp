// Code mods (code_patches.h): what the runtime itself works out for a Cemu graphic pack the game code holds,
// from the manifest tools/botw_code_mods.py writes (code_patches.c) and the pack's folder:
// - the values of its preset variables: Cemu's rules (GraphicPack2.cpp): one active preset per category,
//   a preset is visible when its condition holds, a variable's value comes from an active visible preset,
//   else an active hidden one, else [Default];
// - the programs that compute cave data, game data and instruction immediates from them (postfix, as
//   tools/cemu_patches.py compiles Cemu's expressions: numbers, "$name", "+" "-" "*" "/" "%" "^" "==" "!="
//   "<" "<=" ">" ">=", "fn:ha" "fn:hi" "fn:lo" "fn:u32"), with Cemu's double arithmetic (or its integer
//   arithmetic, for conditions);
// - which folder in the Mods folder is the pack and the fingerprint of its files (the same as the
//   translator's, so a changed pack is noticed).
// No guest memory and no generated code here (content_merge_test-style tests link it on its own).
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "mod_json.h"

namespace mods::patches {

uint32_t to_u32(double v);  // (uint32)double as Cemu's builds convert it (through a 64-bit integer)
// the value of a postfix program; integer: TExpressionParser<int>'s arithmetic. Throws on a missing
// variable or a malformed program.
double evaluate(const json::Value& program, const std::map<std::string, double>& vars, bool integer = false);

struct Preset {
    std::string name, category;
    bool is_default = false;
    json::Value condition;  // Null: always visible
    std::map<std::string, double> values;
};
struct Rules {
    std::map<std::string, double> defaults;
    std::vector<std::string> categories;  // in the pack's order
    std::vector<Preset> presets;          // in the pack's order
};
Rules rules_of(const json::Value& mod);  // from a manifest entry ("variables", "categories", "presets")

struct Selection {
    std::vector<bool> active, visible;  // per preset
};
// the presets in use: per category the chosen one (category -> preset name), else its default (the
// preset marked default, or the first); then Cemu's checks, where a category with visible presets whose
// active one is hidden gets its default visible one
Selection select(const Rules& r, const std::map<std::string, std::string>& chosen);
std::map<std::string, double> values(const Rules& r, const Selection& s);

// the folder with rules.txt in a mod folder (through folders around it), or empty
std::filesystem::path pack_folder(const std::filesystem::path& mod);
// rules.txt and the patch files Cemu reads (every patch_*.asm, else patches.txt), by name
std::vector<std::string> pack_files(const std::filesystem::path& pack);
std::string fingerprint(const std::filesystem::path& pack);  // tools/botw_code_mods.py fingerprint()

}  // namespace mods::patches
