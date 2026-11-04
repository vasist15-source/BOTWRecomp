// MetalFX options: see metalfx_opts.h
#include "metalfx_opts.h"

#include <atomic>
#include <cstdlib>
#include <mutex>

#include "overlay/hostui.h"

namespace mfx {
namespace {
int env_flag(const char* name) {  // -1 unset, else 0/1
    const char* e = getenv(name);
    return e && *e ? (atoi(e) != 0) : -1;
}
const int g_env_up = env_flag("WWHD_METALFX_UPSCALE");
const int g_env_fg = env_flag("WWHD_METALFX_FRAMEGEN");
std::atomic<bool> g_up{g_env_up == 1}, g_fg{g_env_fg == 1};
std::atomic<bool> g_up_ok{false}, g_fg_ok{false};
std::mutex g_status_mutex;
std::string g_status;
}  // namespace

bool upscale() { return g_up.load(std::memory_order_relaxed) && g_up_ok.load(std::memory_order_relaxed); }
bool framegen() { return g_fg.load(std::memory_order_relaxed) && g_fg_ok.load(std::memory_order_relaxed); }
void set_upscale(bool on) {
    g_up = on;
    if (g_env_up < 0) hostui::set("metalfxUpscale", on ? "1" : "0");
}
void set_framegen(bool on) {
    g_fg = on;
    if (g_env_fg < 0) hostui::set("metalfxFrameGen", on ? "1" : "0");
}
bool from_env() { return g_env_up >= 0 || g_env_fg >= 0; }
bool upscale_available() { return g_up_ok; }
bool framegen_available() { return g_fg_ok; }
void set_available(bool up, bool fg) {
    g_up_ok = up;
    g_fg_ok = fg;
}
std::string status() {
    std::lock_guard<std::mutex> lk(g_status_mutex);
    return g_status;
}
void set_status(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_status_mutex);
    g_status = s;
}
void load_saved() {
    std::string v;
    if (g_env_up < 0 && hostui::get("metalfxUpscale", v)) g_up = v == "1";
    if (g_env_fg < 0 && hostui::get("metalfxFrameGen", v)) g_fg = v == "1";
}
}  // namespace mfx
