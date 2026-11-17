// The HUD pane rules (aspect_hud.h): which routine a pane gets, and what the routines do.
#include "aspect_hud.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace aspect::hud;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "aspect_hud_test.cpp:%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
static Action routine(const char* screen, const char* parts, const char* pane, int mode = kEdges, float* offset = nullptr) {
    float o;
    Action a = decide(screen, parts, pane, mode, o);
    if (offset) *offset = o;
    return a;
}
int main() {
    float o;
    // screens whose whole content hangs under N_InOut_00
    CHECK(routine("BootUp_00", "", "N_InOut_00") == a_scalePaneNormal);
    CHECK(routine("BootUp_00", "", "RootPane") == a_exitScale);
    CHECK(routine("NoSuchScreen_00", "", "N_InOut_00") == a_exitScale);
    // the main HUD: by parts pane, then by name
    CHECK(routine("MainScreen_00", "Pa_Sensor_00", "N_InOut_00", kEdges, &o) == a_scalePaneAndKeepCustomPosIf && o == 392.0f);
    CHECK(routine("MainScreen_00", "Pa_Time_00", "N_All_00", kEdges, &o) == a_scalePaneAndKeepCustomPosIf && near(o, 342 * 0.85f));
    CHECK(routine("MainScreen_00", "Pa_Time_00", "N_All_00", kCentred, &o) == a_scalePaneAndKeepCustomPosIf && near(o, 342 / 0.85f));
    CHECK(routine("MainScreen_00", "Pa_Other_00", "N_State_00") == a_scalePaneToLeftSideIf);
    CHECK(routine("MainScreen_00", "Pa_SinJu_00", "Pa_SinJu_00") == a_scalePaneAndKeepPosIf);
    CHECK(routine("MainScreen_00", "Pa_Weather_00", "P_Whatever_00") == a_exitScale);
    // screens that differ with and without a parts pane before
    CHECK(routine("AppMap_00", "", "N_InOut_00") == a_scaleMapAndStore);
    CHECK(routine("AppMap_00", "Pa_Map_00", "B_MapCapture_00") == a_scaleOnlySizeReverse);
    CHECK(routine("PauseMenuRecipe_00", "", "N_InOut_00") == a_scalePaneAndPos);
    CHECK(routine("PauseMenuRecipe_00", "Pa_Item_00", "N_InOut_00") == a_exitScale);
    CHECK(routine("Title_00", "", "N_3_Gake_00") == a_scalePaneOnYScale);
    CHECK(routine("Fade", "", "N_LogoInOut_02") == a_scalePaneToRightSide);
    CHECK(routine("PauseMenu_00", "", "N_InOut_00") == a_scalePaneAndPos);

    // 21:9 (64:27): everything is drawn 4/3 as wide, so panes are scaled by 3/4; the edge is 160 further out
    const Factors edges = factors(64.0f / 27.0f, kEdges), centred = factors(64.0f / 27.0f, kCentred);
    CHECK(near(edges.ar, 0.75f) && near(edges.pad, 160.0f));
    auto run = [](Action a, Pane p, const Factors& f, float offset = 0) { apply(a, p, f, offset); return p; };
    const Pane p{-600, 1, 1, 1280};
    CHECK(near(run(a_scalePaneNormal, p, edges).sx, 0.75f) && run(a_scalePaneNormal, p, edges).x == -600);
    CHECK(near(run(a_scalePaneToLeftSide, p, edges).x, -760) && near(run(a_scalePaneToRightSide, p, edges).x, -440));
    // a pane 40 from the left edge stays 40 from it: (610 * 4/3) is 40 short of the half width 853.3
    CHECK(near(run(a_scalePaneAndKeepPos, p, edges).x, -610) && near(640.0f / 0.75f - 610.0f / 0.75f, 40.0f));
    CHECK(near(run(a_scalePaneAndKeepPosIf, p, edges).x, -610) && near(run(a_scalePaneAndKeepPosIf, p, centred).x, -450));
    CHECK(near(run(a_scalePaneToLeftSideIf, p, edges).x, -760) && run(a_scalePaneToLeftSideIf, p, centred).x == -600);
    CHECK(near(run(a_scalePaneAndPos, p, edges).x, -450) && near(run(a_scaleOnlyPos, p, edges).sx, 1));
    CHECK(near(run(a_scalePaneReverse, p, edges).sx, 4.0f / 3.0f) && near(run(a_scalePaneOnYScale, p, edges).sy, 4.0f / 3.0f));
    CHECK(near(run(a_scaleOnlySizeReverse, p, edges).w, 1280 * 4.0f / 3.0f));
    // a child at 0 under a parent at 392: at the edge it moves with the parent's place, centred it scales it
    const Pane child{0, 1, 1, 0};
    CHECK(near(run(a_scalePaneAndKeepCustomPosIf, child, edges, 392).x, 62) && near(run(a_scalePaneAndKeepCustomPosIf, child, centred, 392).x, -98));
    CHECK(near(run(a_scaleMapAndStore, child, edges).x, 160) && run(a_scaleMapAndStore, child, centred).x == 0);
    CHECK(near(run(a_movePaneToRightSide, child, edges).x, 160) && near(run(a_moveKeepPos, p, edges).x, -610));
    CHECK(run(a_exitScale, p, edges).x == p.x && run(a_exitScale, p, edges).sx == p.sx);
    CHECK(near(sensor_x(392, 0.75f), 392) && near(sensor_x(566, 0.75f), 522.5f));
    std::puts("aspect_hud_test: ok");
}
