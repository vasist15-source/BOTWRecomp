// nn_nfp: amiibo (NFC). No reader: the library initializes and searches, a tag is never found. States
// and result codes follow Cemu's nn_nfp (without an amiibo loaded): the game's amiibo rune keeps
// "searching" and every tag access reports no tag, instead of the unimplemented calls' leftover r3.
#include <atomic>

#include "../runtime.h"

namespace {
enum : uint32_t { NFP_STATE_NONE = 0, NFP_STATE_INIT = 1, NFP_STATE_RW_SEARCH = 2 };
constexpr uint32_t kOk = 0;
constexpr uint32_t kNoTag = (5u << 29) | (27u << 20);  // status level, module NFP (Cemu's "todo: correct error code")
std::atomic<bool> g_init{false}, g_detecting{false};
}  // namespace

HLE(nn_nfp, Initialize__Q2_2nn3nfpFv) {
    if (!g_init.exchange(true)) LOG("[nfp] amiibo library initialized (no reader: tags are never found)");
    ret(c, kOk);
}
HLE(nn_nfp, Finalize__Q2_2nn3nfpFv) {
    g_init = false;
    g_detecting = false;
    ret(c, kOk);
}
HLE(nn_nfp, GetNfpState__Q2_2nn3nfpFv) {
    ret(c, !g_init ? NFP_STATE_NONE : g_detecting ? NFP_STATE_RW_SEARCH : NFP_STATE_INIT);
}
HLE(nn_nfp, StartDetection__Q2_2nn3nfpFv) {
    g_detecting = g_init.load();
    ret(c, g_init ? kOk : kNoTag);
}
HLE(nn_nfp, StopDetection__Q2_2nn3nfpFv) {
    g_detecting = false;
    ret(c, kOk);
}
// the events are signalled when a tag arrives or leaves: never
HLE(nn_nfp, SetActivateEvent__Q2_2nn3nfpFP7OSEvent) { ret(c, kOk); }
HLE(nn_nfp, SetDeactivateEvent__Q2_2nn3nfpFP7OSEvent) { ret(c, kOk); }
// tag access without a tag
HLE(nn_nfp, Mount__Q2_2nn3nfpFv) { ret(c, kNoTag); }
HLE(nn_nfp, MountRom__Q2_2nn3nfpFv) { ret(c, kNoTag); }
HLE(nn_nfp, Unmount__Q2_2nn3nfpFv) { ret(c, kOk); }
HLE(nn_nfp, OpenApplicationArea__Q2_2nn3nfpFUi) { ret(c, kNoTag); }
HLE(nn_nfp, ReadApplicationArea__Q2_2nn3nfpFPvUi) { ret(c, kNoTag); }
HLE(nn_nfp, GetTagInfo__Q2_2nn3nfpFPQ3_2nn3nfp7TagInfo) { ret(c, kNoTag); }
HLE(nn_nfp, GetNfpCommonInfo__Q2_2nn3nfpFPQ3_2nn3nfp10CommonInfo) { ret(c, kNoTag); }
HLE(nn_nfp, GetNfpRegisterInfo__Q2_2nn3nfpFPQ3_2nn3nfp12RegisterInfo) { ret(c, kNoTag); }
HLE(nn_nfp, GetNfpRomInfo__Q2_2nn3nfpFPQ3_2nn3nfp7RomInfo) { ret(c, kNoTag); }
// the amiibo settings applet is not available
HLE(nn_nfp, InitializeAmiiboSettingsArgsIn__Q2_2nn3nfpFPQ3_2nn3nfp20AmiiboSettingsArgsIn) { ret(c, kOk); }
HLE(nn_nfp, SwitchToAmiiboSettings__Q2_2nn3nfpFRCQ3_2nn3nfp20AmiiboSettingsArgsInPCcUi) {
    LOG("[nfp] amiibo settings requested: not available");
    ret(c, kNoTag);
}
// (const Result&) -> the number shown to the player (Cemu's mapping)
HLE(nn_nfp, GetErrorCode__Q2_2nn3nfpFRCQ2_2nn6Result) {
    const uint32_t result = arg(c, 0) ? ld32(arg(c, 0)) : 0;
    const uint32_t level = (result >> 27) & 3;
    uint32_t code = 1680000;
    if ((result & (level != 3 ? 0x1FF00000u : 0x7F00000u)) == 0x1B00000u && (result & 0x80000000u))
        code = ((result & (level != 3 ? 0xFFFFFu : 0x3FFu)) >> 7) + 1680000;
    ret(c, code);
}
