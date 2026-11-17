// platform/controller_picture.mm: the controller pictures of Settings > Controls, read from this executable's
// own __PADS sections (linked in as wwhd's are) and decoded: each one is there, 1000 pixels wide, clear
// around the controller and solid on it, and nothing else is found. --rgba <dir> also writes
// <dir>/<name>.rgba (int32 width, int32 height, RGBA8) for controls_view_test --pictures.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace host { bool controller_picture(const char* name, int& width, int& height, std::vector<unsigned char>& rgba); }

static int g_fail = 0, g_pass = 0;
#define CHECK(x)                                                              \
    do {                                                                      \
        if (x) g_pass++;                                                      \
        else { g_fail++; fprintf(stderr, "%s:%d: FAILED: %s (%s)\n", __FILE__, __LINE__, #x, name); } \
    } while (0)

int main(int argc, char** argv) {
    const char* out = argc > 2 && !strcmp(argv[1], "--rgba") ? argv[2] : nullptr;
    // name, the height controls_view.cpp's picture_def has for it, a point on the controller (a stick's cap)
    const struct { const char* name; int height, x, y; } pictures[] = {
        {"xbox", 753, 248, 212}, {"dualsense", 667, 345, 375}, {"dualshock4", 637, 342, 385}, {"switchpro", 675, 258, 217}};
    bool straight = false;
    for (const auto& p : pictures) {
        const char* name = p.name;
        int w = 0, h = 0;
        std::vector<unsigned char> rgba;
        CHECK(host::controller_picture(name, w, h, rgba));
        CHECK(w == 1000 && h == p.height);  // the button positions are pixels of this size
        if (w != 1000 || h != p.height) continue;
        CHECK(rgba.size() == (size_t)w * h * 4);
        auto px = [&](int x, int y) { return &rgba[((size_t)y * w + x) * 4]; };
        // clear in the corners (the cut-out background), solid on the controller
        CHECK(px(1, 1)[3] == 0 && px(w - 2, 1)[3] == 0 && px(1, h - 2)[3] == 0);
        CHECK(px(p.x, p.y)[3] == 255);
        // soft edges; and not premultiplied: an edge pixel's colour is not darkened by its alpha (some pixel
        // is half clear and still has a channel above its alpha: a white outline's edge. A drawing edged in
        // black has none, so this is asked of the set, below)
        bool partial = false;
        size_t solid = 0;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            const unsigned char* q = &rgba[i * 4];
            solid += q[3] == 255;
            if (q[3] > 40 && q[3] < 200) {
                partial = true;
                straight |= q[0] > q[3] || q[1] > q[3] || q[2] > q[3];
            }
        }
        CHECK(partial);
        CHECK(solid > (size_t)w * h / 4 && solid < (size_t)w * h * 9 / 10);  // a controller, not a rectangle
        if (out) {
            if (FILE* f = fopen((std::string(out) + "/" + name + ".rgba").c_str(), "wb")) {
                const int32_t size[2] = {w, h};
                fwrite(size, 4, 2, f);
                fwrite(rgba.data(), 1, rgba.size(), f);
                fclose(f);
            }
        }
    }
    const char* name = "all";
    CHECK(straight);
    name = "steam";  // no picture: the page draws that one
    int w, h;
    std::vector<unsigned char> rgba;
    CHECK(!host::controller_picture(name, w, h, rgba));
    printf("controller_picture_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
