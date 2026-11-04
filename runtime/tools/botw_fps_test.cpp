#include "ppc.h"
#include "botw_fps.h"
#include "../src/gx2/fps_limiter.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sys/mman.h>
#include <vector>

namespace {
constexpr uint32_t actor=0x60010000, vfr=0x60001000, params=0x60021000,
                   properties=0x60023000, stack=0x600F0000, text=0x60030000;
double consumed;
unsigned drains, finishes;
bool aiming=true, running=true;
void near(double a,double b) { if(std::abs(a-b)>1e-5*std::max(1.0,std::abs(b))) {
    fprintf(stderr,"actual %.10g expected %.10g\n",a,b); abort(); } }
}
extern "C" {
int g_ppc_trace=0;
volatile int g_core_preempt[3]{};
void ppc_trace_enter(uint32_t) { abort(); }
void ppc_preempt(Cpu*) { abort(); }
void ppc_unimplemented(Cpu*,uint32_t,uint32_t) { abort(); }
void f_02D5F600(Cpu*);
void f_02D90A54(Cpu*);
void f_037924B4(Cpu*);
void imp_coreinit_OSGetCoreId(Cpu* c) { c->r[3]=0; }
void f_037966A8(Cpu* c) { c->r[3]=running; }
void f_02D33CBC(Cpu* c) { c->r[3]=aiming; }
void f_03792F28(Cpu* c) { assert(c->r[3]==vfr && c->r[4]==1); }
void f_02D8F970(Cpu* c) { c->r[3]=1; }
// The actual caller and /30 conversion run. Observe the amount passed to
// the final common stamina handler; do not simulate its actor/status system.
void f_02D908B4(Cpu* c) { assert(c->r[3]==actor); consumed+=c->f[1].ps0; ++drains; }
void ppc_dispatch(Cpu* c) { assert(c->pc==0x12340000 && c->r[3]==actor); ++finishes; }
#define UNUSED(name) void f_##name(Cpu*) { abort(); }
UNUSED(02D33884) UNUSED(02D353E0) UNUSED(02D35928) UNUSED(02D43414)
UNUSED(02DE42BC) UNUSED(02E1AE9C) UNUSED(03072950) UNUSED(034C04F4)
UNUSED(0370E458)
#define SITE(name) void site_##name(Cpu*);
SITE(02D5F760) SITE(02D90D30) SITE(02D90D8C) SITE(02C6B8F8)
SITE(020E551C) SITE(020E731C) SITE(020E3EB8) SITE(020E5398)
SITE(033609E0) SITE(02B951F0) SITE(0348BA3C) SITE(022AC944)
SITE(0209FFC0) SITE(0333D350) SITE(0333D80C) SITE(0333DC78)
SITE(0333DD58) SITE(034A804C) SITE(031D2D2C) SITE(031D3798)
SITE(022D0F30) SITE(022D1098)
}
static Cpu cpu() {
    Cpu c{}; c.r[1]=stack;c.lr=0xABCDE000;
    for(unsigned i=14;i<32;++i) {c.r[i]=0x90000000+i;c.f[i].ps0=i+.25;c.f[i].ps1=i+.5;}
    return c;
}
static void ratio(double value) { stf32(vfr+0x1C,value); }
static void bow_frame(double step) {
    ratio(step);Cpu c=cpu();c.r[3]=actor;f_02D5F600(&c);
    assert(c.r[1]==stack && c.lr==0xABCDE000);
    for(unsigned i=14;i<32;++i) assert(c.r[i]==0x90000000+i);
    for(unsigned i=28;i<32;++i) {near(c.f[i].ps0,i+.25);near(c.f[i].ps1,i+.5);}
}
static void init_bow() {
    st32(actor+0x83C,0x2000); // arrow-time branch
    st32(actor+0x39C,0x60020000);st32(0x60020078,params);
    st32(params+0x244,0x60022000);st32(0x60022000,properties);
    stf32(properties+0xFC,60); // synthetic cost per 30-Hz second
    stf32(0x101E5534,0.00000011920928955078125);stf32(0x101E5364,1);
    stf32(0x101E423C,1);stf32(0x101E7960,1./30);
    st32(actor+0xE8,0x60024000);st32(0x6002472C,0x12340000);
}
static void bow_tests(bool legacy) {
    init_bow();
    for(int fps : {20,30,45,60,90,120,240}) {
        consumed=0;drains=finishes=0;
        for(int n=0;n<fps;++n) bow_frame(30./fps);
        printf("actual bow caller + /30 conversion: %d fps, %.6f units/second\n",fps,consumed);
        near(consumed,legacy ? 2.*fps : 60.);
        assert(drains==static_cast<unsigned>(fps) && finishes==drains);
    }
    if(legacy) return;
    for(auto steps : std::vector<std::vector<double>>{
          [] {std::vector<double> s(15,1);s.insert(s.end(),45,1./3);return s;}(),
          [] {std::vector<double> s;for(int i=0;i<30;++i){s.push_back(.4);s.push_back(.6);}return s;}()}) {
        consumed=0;for(double step:steps) bow_frame(step);near(consumed,60);
    }
    consumed=0;aiming=false;st32(actor+0x83C,0);bow_frame(.25);assert(consumed==0);
    aiming=true;st32(actor+0x83C,0x2000);running=false;bow_frame(.25);assert(consumed==0);running=true;
    stf32(properties+0xFC,0);bow_frame(.25);assert(consumed==0);stf32(properties+0xFC,60);
    // Other callers keep the original one-off cost and optional actor multiplier.
    ratio(.25);consumed=0;Cpu c=cpu();c.r[3]=actor;c.r[4]=1;c.f[1].ps0=60;f_02D90A54(&c);near(consumed,2);
    c=cpu();c.r[3]=actor;c.r[4]=0;c.f[1].ps0=60;stf32(actor+0x1B18,3);f_02D90A54(&c);near(consumed,8);
    st32(0x1047C258,0);c=cpu();c.r[11]=properties;site_02D5F760(&c);near(c.f[1].ps0,60);st32(0x1047C258,vfr);
}
static void site_tests() {
    for(double fps : {20.,30.,45.,60.,90.,120.,240.}) {
        const double step=30./fps;ratio(step);
        Cpu c=cpu();auto original=c;
        site_02D90D30(&c);near(60/c.f[11].ps0*fps,60);
        site_02D90D8C(&c);near(60/c.f[11].ps0*fps,60);
        c.f[13].ps0=4;c.f[29].ps0=3/step;site_02C6B8F8(&c);near(c.f[13].ps0*step,4);assert(c.cr[1]);
        site_020E551C(&c);near(c.f[11].ps0*fps,900);
        site_020E731C(&c);near(c.f[13].ps0*fps,900);
        site_020E3EB8(&c);near(c.f[13].ps0*step,.5);
        site_020E5398(&c);near(c.f[13].ps0*step,.5);
        site_033609E0(&c);near(c.f[0].ps0*fps,30);
        site_02B951F0(&c);near(c.f[11].ps0*fps,45);
        c.f[27].ps0=1;c.f[13].ps0=1/fps;site_0348BA3C(&c);near(c.f[25].ps0,30);
        c.f[12].ps0=12;site_022AC944(&c);near((c.f[0].ps0-12)*fps,30);
        c.r[9]=0x10010000;stf32(0x100136B4,12);site_0209FFC0(&c);near(c.f[0].ps0*fps,360);
        c.r[25]=0x60050000;stf32(c.r[25]+0x48,1/fps);site_0333D350(&c);near(c.f[12].ps0,0.03333334f);
        c.f[31].ps0=10;c.f[30].ps0=9;c.f[9].ps0=step;site_0333D80C(&c);near(c.f[9].ps0,10);
        c.f[9].ps0=step;site_0333DC78(&c);near(c.f[9].ps0,9);
        c.f[9].ps0=step;site_0333DD58(&c);near(c.f[9].ps0,9);
        for(double property : {0.,.5,1.,2.}) {
            c.f[30].ps0=step;c.f[1].ps0=property;site_034A804C(&c);
            near(c.f[30].ps0,((step-1)*.45+1)*property);
        }
        // Both initialization/reversal paths use the workaround only above
        // 30 Hz. Run the real engine timer to verify the previously delayed
        // phase completes on the first frame, as in the 30-Hz baseline.
        st32(vfr+0xC0,0x60051000);st32(0x1054ABA0,0);stf32(0x60051000,step);
        for(auto site : {site_022D0F30,site_022D1098}) {
            Cpu t=cpu();t.r[12]=0x10070000;site(&t);
            const double decrement=site==site_022D0F30?t.f[31].ps0:t.f[0].ps0;
            near(decrement,step<1?-999:-1);
            stf32(0x60052000,1);stf32(0x60052008,decrement);
            t.r[3]=0x60052000;f_037924B4(&t);assert(ldf32(0x60052000)<=0);
            assert(t.r[1]==stack && t.lr==0xABCDE000);
        }
        // Patches may only change their designated FP results and comparison.
        c.r[9]=original.r[9];c.r[25]=original.r[25];
        assert(memcmp(c.r,original.r,sizeof(c.r))==0 && c.lr==original.lr && c.ctr==original.ctr);
    }
    for(double invalid : {0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        ratio(invalid);near(botw_frame_ratio(),1);
    }
    puts("actual generated FPS sites: stamina, rush, arrows, shield, runes, korok, motorcycle, ragdoll, water passed");
}
static void event_begin(uint32_t object,uint32_t id,const char* name) {
    strcpy(reinterpret_cast<char*>(ppc_ptr(text)),name);
    Cpu c=cpu();c.r[31]=object;c.r[9]=id;c.r[29]=params;st32(params+8,text);
    auto before=c;site_031D2D2C(&c);before.r[12]=text;
    assert(memcmp(&c,&before,sizeof c)==0);
}
static void event_end(uint32_t object,uint32_t id) {
    st32(object+0x274,id);st32(object+0x27C,0xAABBCCDD);
    Cpu c=cpu();c.r[30]=object;auto before=c;site_031D3798(&c);before.r[11]=0xAABBCCDD;
    assert(memcmp(&c,&before,sizeof c)==0);
}
static void cutscene_tests() {
    for(int code : {3480,3481,1620,1630,3340,1690,1100,1110,1120,1130,1140,1150,1160,1170,1180,1190,1200,1210,1220,1230,1240,1250,1030}) {
        char name[16];snprintf(name,sizeof name,"Demo%03d_%d",code/10,code%10);
        event_begin(actor,code,name);assert(botw_cutscene_limit()==(code==1030?60:30));
        for(int requested : {0,30,60,90,120}) {
            int expected=requested==0 ? botw_cutscene_limit() : std::min(requested,botw_cutscene_limit());
            assert(botw_effective_fps(requested)==expected);
            assert(gx2::frame_limit(botw_effective_fps(requested),true,0)==expected);
        }
        event_end(actor,code);assert(botw_cutscene_limit()==0 && botw_effective_fps(90)==90);
    }
    event_begin(actor,1,"Demo103_0");event_begin(actor+0x400,2,"Demo348_1");
    event_begin(actor+0x800,3,"Demo999_0");event_end(actor+0x800,3);assert(botw_cutscene_limit()==30);
    event_end(actor+0x400,999);assert(botw_cutscene_limit()==30); // wrong generation
    event_end(actor+0x400,2);assert(botw_cutscene_limit()==60);
    event_end(actor,1);event_end(actor,1);assert(botw_cutscene_limit()==0);
    for(const char* name : {"", "D", "Demo", "Demo348", "Demo348_x", "Demo348_2", "Talk", "Demo102_0"}) {
        event_begin(actor,8,name);assert(botw_cutscene_limit()==0);
    }
    event_begin(actor,9,"Demo348_0");event_begin(actor,10,"Talk");assert(botw_cutscene_limit()==0);
    puts("actual event hook sites + native caps: listed events, user limits, nesting, destruction, reuse and register preservation passed");
}
int main(int argc,char** argv) {
    for(uint32_t a : {0x10000000u,0x60000000u}) assert(mmap(ppc_ptr(a),0x1000000,
        PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==ppc_ptr(a));
    st32(0x1047C258,vfr);st32(vfr+0x10,0x10319D4C);
    bow_tests(argc>1 && strcmp(argv[1],"legacy")==0);
    if(argc==1) {site_tests();cutscene_tests();}
}
