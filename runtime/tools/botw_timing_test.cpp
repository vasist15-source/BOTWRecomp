#include "ppc.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <vector>
#include <utility>

extern "C" {
int g_ppc_trace = 0;
volatile int g_core_preempt[3]{};
void ppc_trace_enter(uint32_t) { abort(); }
void ppc_preempt(Cpu*) { abort(); }
void ppc_unimplemented(Cpu*, uint32_t, uint32_t) { abort(); }
void f_030BD694(Cpu*);
void f_037DC2F8(Cpu*);
uint32_t botw_menu_countdown(uint32_t, uint32_t);
static unsigned core;
void imp_coreinit_OSGetCoreId(Cpu* c) { c->r[3] = core; }
void f_03BB14C0(Cpu* c) { c->r[3] = 0x60003000; }
void f_030AB9A0(Cpu* c) { assert(c->f[1].ps0 == 0); c->r[3] = 0; } // atan(0), cardinal stick fixture
void f_02F62A6C(Cpu*);
static double map_scale=1, map_zoom=1;
static unsigned map_mode=0, map_writes=0;
void f_02EA91F8(Cpu* c) { c->f[1].ps0=map_scale; }
void f_02EA9E00(Cpu* c) { c->r[3]=0x60050000; }
void f_02E90BFC(Cpu* c) { c->f[1].ps0=map_zoom; }
void f_02EAA15C(Cpu* c) { c->r[3]=map_mode; }
void f_02EA9FB8(Cpu* c) { assert(map_mode==1); st64(0x60050000,ld64(c->r[4])); ++map_writes; }
void f_02EA9F7C(Cpu* c) { assert(map_mode==0); st64(0x60050000,ld64(c->r[4])); ++map_writes; }
void f_02EA9B68(Cpu* c) { assert(c->r[4]==1); }
}
constexpr uint32_t vfr = 0x60001000, delta = 0x60002000, audio = 0x60004000;
static Cpu cpu() { Cpu c{}; c.r[1] = 0x600F0000; return c; }
static void ratio(double step) { stf32(vfr + 0x1C, step); }
static void map_frame(double step, double x, double y) {
    ratio(step);
    stf32(0x60031BA4,x); stf32(0x60031BA8,y);
    Cpu c=cpu(); c.r[3]=0x60030000;
    f_02F62A6C(&c);
}
static void map_tests(bool legacy) {
    stf32(0x1022F880,1);
    st32(0x104698F8,0x60060000); st32(0x60031B68,0x60040000);
    if (legacy) {
        for (int fps : {30,60,90,120}) {
            st64(0x60050000,0);
            for(int i=0;i<fps;++i) map_frame(30./fps,1,0);
            printf("legacy map: %d fps -> %.1f units/second (expected 30)\n",fps,ldf32(0x60050000));
            assert(ldf32(0x60050000)==fps);
        }
        return;
    }
    for (map_mode=0;map_mode<2;++map_mode)
      for (double scale : {0.25,1.,3.})
       for (double zoom : {.5,1.,4.})
        for (auto [x,y] : std::vector<std::pair<double,double>>{{1,0},{0,-1},{.25,.5},{-1,1}})
         for (int fps : {20,30,45,60,90,120,240}) {
            map_scale=scale; map_zoom=zoom;
            st64(0x60050000,0); map_writes=0;
            for(int i=0;i<fps;++i) map_frame(30./fps,x,y);
            assert(std::abs(ldf32(0x60050000)-30*x*scale/zoom)<.002);
            assert(std::abs(ldf32(0x60050004)-30*y*scale/zoom)<.002);
            assert(map_writes==static_cast<unsigned>(fps));
            const uint64_t before=ld64(0x60050000);
            map_frame(30./fps,0,0);
            assert(ld64(0x60050000)==before && map_writes==static_cast<unsigned>(fps));
         }
    map_mode=0; map_scale=map_zoom=1;
    for (auto steps : std::vector<std::vector<double>>{
           [] {std::vector<double> s(15,1);s.insert(s.end(),45,1./3);return s;}(),
           [] {std::vector<double> s(60,.25);s.insert(s.end(),15,1);return s;}(),
           [] {std::vector<double> s;for(int i=0;i<30;++i){s.push_back(.4);s.push_back(.6);}return s;}()}) {
        st64(0x60050000,0);
        for(double step:steps) map_frame(step,1,-1);
        assert(std::abs(ldf32(0x60050000)-30)<.0001);
        assert(std::abs(ldf32(0x60050004)+30)<.0001);
    }
    // No VFR during startup: retain the original one-frame distance.
    st32(0x1047C258,0); st64(0x60050000,0); map_frame(.5,1,0);
    assert(ldf32(0x60050000)==1); st32(0x1047C258,vfr);
    puts("actual map function: frame rates, switches, zoom, direction, both modes, idle and startup fallback passed");
}
static double audio_step(double step, double raw, unsigned cpu_core = 0) {
    core = cpu_core;
    ratio(step);
    stf32(delta, raw);
    Cpu c = cpu();
    f_037DC2F8(&c);
    assert(std::abs(ldf32(0x6000301C) - ldf32(audio + 0x5C)) < 1e-6);
    return ldf32(audio + 0x5C);
}
static void init(uint32_t controller, unsigned count = 2) {
    memset(ppc_ptr(controller), 0, 0x140);
    st32(controller + 0xF8, count);
    st32(controller + 0xFC, -1);
    st32(controller + 0x100, -1);
    for (unsigned i = 0; i < count; ++i) {
        st8(controller + 0xA8 + i, 15);
        st8(controller + 0xC8 + i, 4);
    }
}
static uint32_t input(uint32_t controller, uint32_t held, double step) {
    ratio(step);
    const auto previous = ld32(controller + 0x10C);
    st32(controller + 0x10C, held);
    Cpu c = cpu(); c.r[3] = controller; c.r[4] = previous;
    f_030BD694(&c);
    assert(ld32(controller) == (held & ~previous));
    assert(ld32(controller + 4) == (previous & ~held));
    return ld32(controller + 8);
}

int main(int argc, char** argv) {
    for (uint32_t address : {0x10220000u, 0x10270000u, 0x10310000u, 0x103C0000u,
                            0x10460000u, 0x10470000u, 0x10540000u, 0x60000000u}) {
        size_t size = address == 0x60000000 ? 0x100000 : 0x10000;
        assert(mmap(ppc_ptr(address), size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == ppc_ptr(address));
    }
    st32(0x1047C258, vfr); st32(vfr + 0x10, 0x10319D4C);
    st32(vfr + 0xE8, 30); st32(0x1047F028, audio);
    for (unsigned i = 0; i < 3; ++i) {
        st32(vfr + 0x3C + 4*i, delta); st32(0x1054ABA0 + 4*i, i);
    }
    stf32(0x1031E2C0, 1); st64(0x1031E2E8, 0x4330000000000000ULL);
    stf32(0x60003018, 30);
    if (argc>1 && strcmp(argv[1],"legacy-map")==0) { map_tests(true); return 0; }
    if (argc==1) map_tests(false);
    const uint32_t pad = 0x60010000, other = 0x60011000;
    init(pad);
    if (argc > 1) {
        unsigned repeats = 0;
        for (unsigned i = 0; i < 60; ++i) repeats += !!input(pad, 1, .5);
        const double old = audio_step(.5, .5);
        printf("legacy: 60fps audio step %.1f (expected .5), repeats %u (expected 4)\n", old, repeats);
        assert(old == 1 && repeats == 12);
        return 0;
    }
    for (double fps : {30., 60., 90., 120., 55., 45., 20.}) {
        for (unsigned i = 0; i < 3; ++i)
            assert(std::abs(audio_step(30/fps, 0, i) * fps - 30) < 1e-5);
        assert(std::abs(audio_step(30/fps, 30/fps) * fps - 30) < 1e-5);
        assert(audio_step(30/fps, 2) == 2); // never suppress a larger engine step
    }
    // Same wall-clock time, including switching and varying frame rates.
    for (const auto& steps : std::vector<std::vector<double>>{
             std::vector<double>(30, 1), std::vector<double>(60, .5),
             std::vector<double>(90, 1./3),
             std::vector<double>(120, .25), [] { std::vector<double> s(15, 1); s.insert(s.end(),30,.5); return s; }(),
             [] { std::vector<double> s; for(int i=0;i<30;++i){s.push_back(.4);s.push_back(.6);} return s; }()}) {
        init(pad); input(pad, 0, 1);
        unsigned repeats = 0;
        for (double step : steps) repeats += !!input(pad, 1, step);
        assert(ld32(pad + 0x28) == 30 && repeats == 4);
        assert(input(pad, 0, .5) == 0 && ld32(pad + 0x28) == 0);
    }
    // Immediate short taps, independent buttons/controllers and disabled repeat.
    init(pad); init(other);
    for (int i=0; i<20; ++i) {
        assert(input(pad, 1, .5) == 0);
        assert(input(pad, 0, .5) == 0);
    }
    for (int i=0; i<60; ++i) {
        input(pad, 3, .5); input(other, 1, .5);
    }
    assert(ld32(pad+0x28)==30 && ld32(pad+0x2C)==30 && ld32(other+0x28)==30);
    st8(pad+0xC8, 0);
    for(int i=0;i<30;++i) assert(!(input(pad, 1, 1) & 1));
    // Exact original 30fps repeat sequence, not just the total count.
    init(pad); input(pad, 0, 1);
    for (unsigned frame=0; frame<400; ++frame)
        assert(!!input(pad, 1, 1) == (frame>=15 && (frame-15)%4==0));
    // The same repeat clock receives virtual direction bits from the actual
    // stick-mask routine, while centered sticks release immediately at 60fps.
    init(pad, 32);
    st32(pad+0xFC, 16);
    stf32(pad+0xE8, .5); stf32(pad+0xF0, .25);
    stf32(0x1027AEB4, .5); stf32(0x1027AEC0, 3);
    stf32(pad+0x118, 1);
    unsigned stick_repeats=0;
    for (unsigned i=0;i<60;++i) {
        ratio(.5); Cpu c=cpu(); c.r[3]=pad; c.r[4]=ld32(pad+0x10C);
        f_030BD694(&c);
        assert(ld32(pad+0x10C)==(1u<<19));
        assert(ld32(pad)==(i==0 ? 1u<<19 : 0));
        stick_repeats+=!!ld32(pad+8);
    }
    assert(stick_repeats==4);
    stf32(pad+0x118, 0);
    { Cpu c=cpu(); c.r[3]=pad; c.r[4]=1u<<19; f_030BD694(&c); }
    assert(ld32(pad+0x10C)==0 && ld32(pad+4)==(1u<<19));
    // Long-press acceleration retains the existing countdown and terminal -1.
    for (unsigned fps : {30u,60u,90u,120u}) {
        uint32_t value=29, address=0x60020000+fps*4;
        ratio(30./fps);
        for (unsigned i=0;i<fps;++i) {
            value=botw_menu_countdown(address,value);
            assert(i+1==fps ? value==UINT32_MAX : value!=UINT32_MAX);
        }
    }
    puts("actual guest audio/controller functions: timing, edges, release, switches, isolation and countdown passed");
}
