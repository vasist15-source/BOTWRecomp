// Cemu graphic packs bundled with the runtime (runtime/data/graphic_packs, CC0, compiled in): their
// Metal shader replacements (<base hash>_<aux hash>_ps_msl.txt) with the presets chosen in
// Settings > Graphics. Choices are read once, at the first shader translation; a change applies from
// the next start.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace graphic_packs {

struct Category {
    std::string name;
    std::vector<std::string> presets;  // rules.txt order
    std::string fallback;              // the preset marked default (or the first)
};
struct Pack {
    std::string id, name, description;
    std::vector<Category> categories;
};

const std::vector<Pack>& packs();
std::string selected(const Pack& pack, const Category& category);  // saved choice, or the default
void select(const Pack& pack, const Category& category, const std::string& preset);
bool restart_needed();  // a choice differs from the ones this session's shaders use

bool has_shaders();
// a pack replaces some variant of this program (Cemu base hash): translate it with Cemu's legacy
// pixel uniform layout (fragCoordScale always present), which the pack shaders were written against
bool has_base(uint64_t base, bool vertex);
// replacement source for a shader by Cemu's hashes (Metal flavour), or empty
std::string shader_source(uint64_t base, uint64_t aux, bool vertex);

}  // namespace graphic_packs
