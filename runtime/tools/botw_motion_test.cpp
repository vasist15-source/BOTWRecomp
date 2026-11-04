// BotW motion regressions: real GameController -> fusion -> VPAD path, no game or host devices.
#include "motion/motion.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>
using namespace motion;
void log_msg(const char*, ...) {}
namespace mods { double game_time() { return 0; } }
static constexpr double pi=3.14159265358979323846;
static bool near(double a,double b,double eps=0.0005) { return std::fabs(a-b)<eps; }
struct Matrix {
    double m[3][3]={{1,0,0},{0,1,0},{0,0,1}};
    Matrix times(const Matrix& b) const {
        Matrix out;
        for(int i=0;i<3;i++)for(int j=0;j<3;j++) {
            out.m[i][j]=0;for(int k=0;k<3;k++)out.m[i][j]+=m[i][k]*b.m[k][j];
        }
        return out;
    }
    void rotate(int axis,double angle) {
        Matrix r;int a=(axis+1)%3,b=(axis+2)%3;
        r.m[a][a]=r.m[b][b]=std::cos(angle);r.m[a][b]=-std::sin(angle);r.m[b][a]=std::sin(angle);
        *this=times(r);
    }
};
static double component(Vec3 v,int i) {return i==0?v.x:i==1?v.y:v.z;}
static void check_pose(VpadMotion initial,VpadMotion result,Matrix physical) {
    // VPAD uses the reflected H basis (x,y,-z). Compare relative matrices so source switching
    // never needs to reset the user's accumulated pose. Independent scalar matrix reference.
    const double sign[3]={1,1,-1};
    for(int i=0;i<3;i++)for(int j=0;j<3;j++)
        assert(near(initial.dir[i].dot(result.dir[j]),sign[i]*physical.m[i][j]*sign[j]));
    for(int i=0;i<3;i++)for(int j=0;j<3;j++)assert(near(result.dir[i].dot(result.dir[j]),i==j));
}
static void migration() {
    Settings old;old.mapping_version=0;old.tuning.axis=kPlayerSpace;
    old.tuning.sensitivity_x=old.tuning.sensitivity_y=.5f;old.tuning.invert_x=true;
    upgrade_controller_mapping(old);
    assert(old.tuning.axis==kNativeAxes && old.tuning.sensitivity_x==1 && old.tuning.sensitivity_y==.5f && old.tuning.invert_x);
    old.tuning.sensitivity_x=.5f; // a deliberate new choice must survive restart
    auto chosen=old;upgrade_controller_mapping(old);assert(old==chosen);
    for(int source:{kController,kCemuhook,kOff,kMouse})for(int mode:{kPlayerSpace,kYawAxis,kRollAxis}) {
        Settings s;s.source=source;s.mapping_version=0;s.tuning.axis=mode;s.tuning.sensitivity_x=.73f;s.tuning.sensitivity_y=1.37f;
        upgrade_controller_mapping(s);
        assert(s.source==source && s.tuning.sensitivity_x==.73f && s.tuning.sensitivity_y==1.37f);
        assert(s.tuning.axis==(source!=kMouse && mode==kPlayerSpace?kNativeAxes:mode));
    }
    Settings round;for(const char* key:kKeys)from_kv(round,key,value_of(chosen,key));assert(round==chosen);
    assert(source_from_id("controller")==kController && axis_from_id("native")==kNativeAxes);
}
static void golden_axes() {
    // Direction vectors match Cemu's documented physical VPAD captures (MotionSample.h).
    // All three physical 45-degree turns must produce the same amount of motion at 1x.
    for(int axis=0;axis<3;axis++)for(int direction:{-1,1}) {
        VirtualPad pad;auto before=pad.vpad();Matrix physical;physical.rotate(axis,direction*pi/4);
        Aim turn; if(axis==0)turn.pitch=direction*pi/4;else if(axis==1)turn.yaw=direction*pi/4;else turn.roll=direction*pi/4;
        pad.turn(.5f,Tuning{}.apply(turn));auto after=pad.vpad();check_pose(before,after,physical);
        assert(near(component(after.gyro,axis),direction*(axis==2?1:-1)*.25));
        for(int other=0;other<3;other++)if(other!=axis)assert(near(component(after.gyro,other),0));
        assert(near(after.acc_magnitude,1));assert(near(pad.vpad().gyro.length(),0));
    }
    Tuning t;t.sensitivity_x=2;t.sensitivity_y=.5;t.invert_x=true;t.invert_y=true;
    auto a=t.apply({1,1,1});assert(a.yaw==-2 && a.pitch==-.5 && a.roll==-2);
    float rate[3]={.2f,.3f,.4f},acc[3]={.1f,.2f,-.9f};Vec3 g,a_gc;
    from_gamecontroller(rate,acc,g,a_gc);assert(near(g.x,.2)&&near(g.y,-.4)&&near(g.z,.3));
    assert(near(a_gc.x,.1)&&near(a_gc.y,.9)&&near(a_gc.z,.2));
    float sdl_g[3]={rate[0],rate[2],-rate[1]},sdl_a[3]={-acc[0]*9.80665f,-acc[2]*9.80665f,acc[1]*9.80665f};Vec3 gs,as;
    from_sdl(sdl_g,sdl_a,gs,as);assert((g-gs).length()<1e-6 && (a_gc-as).length()<1e-6);
    // DSU and GameController report the same physical turn with their own axis/unit conventions.
    float gd[3]={g.x*180/float(pi),g.y*180/float(pi),g.z*180/float(pi)},ad[3]={a_gc.x,-a_gc.y,-a_gc.z};
    from_dsu(gd,ad,gs,as);assert((g-gs).length()<1e-6 && (a_gc-as).length()<1e-6);
}
static void production(int fps) {
    Settings settings;settings.source=kController;set_settings(settings);
    uint64_t stamp=1000000000;constexpr int hz=240;constexpr double dt=1.0/hz;
    float zero[3]={},flat[3]={0,0,-1};
    controller_sample_gamecontroller(71,stamp,zero,flat);auto initial=vpad(false),last=initial;
    Matrix physical;
    for(int frame=1;frame<=hz*3;frame++) {
        // Alternating short X/Y/Z turns within the same frame exposes lost integration order.
        int axis=((frame-1)/2)%3;int direction=((frame-1)/120)%2?-1:1;
        double radians=direction*pi*dt;physical.rotate(axis,radians);
        Vec3 w; if(axis==0)w.x=direction*pi;else if(axis==1)w.y=direction*pi;else w.z=direction*pi;
        float gyro[3]={w.x,w.z,-w.y};
        // world gravity expressed in the controller's physical H frame, then Apple's frame
        float accel[3]={(float)physical.m[1][0],(float)physical.m[1][2],(float)-physical.m[1][1]};
        stamp=1000000000+uint64_t(std::llround(frame*1e9/hz));
        controller_sample_gamecontroller(71,stamp,gyro,accel);
        if(frame*fps/hz!=(frame-1)*fps/hz) {
            last=vpad(false);check_pose(initial,last,physical);
            auto repeated=vpad(true);for(int i=0;i<3;i++)assert((last.dir[i]-repeated.dir[i]).length()<1e-7);
            assert((last.gyro-repeated.gyro).length()<1e-7);
        }
    }
    last=vpad(false);check_pose(initial,last,physical);
    // The latest angular rate remains valid between sensor packets; stopping needs a zero packet.
    controller_sample_gamecontroller(71,stamp+=4166667,zero,flat);
    last=vpad(false);assert(near(last.gyro.length(),0));
    controller_gone(71);auto disconnect=vpad(false);for(int i=0;i<3;i++)assert((last.dir[i]-disconnect.dir[i]).length()<1e-6);
    controller_sample_gamecontroller(72,stamp+100000000,zero,flat);auto reconnect=vpad(false);
    for(int i=0;i<3;i++)assert((last.dir[i]-reconnect.dir[i]).length()<1e-6);
    settings.source=kOff;set_settings(settings);
    float turn[3]={1,2,3};controller_sample_gamecontroller(72,stamp+104000000,turn,flat);
    auto off=vpad(false);for(int i=0;i<3;i++)assert((last.dir[i]-off.dir[i]).length()<1e-6);
    printf("PASS: native three-axis GameController -> VPAD, %d FPS, ordered compound turns, repeat/stop/reconnect/off; migration and Cemu physical-axis reference\n",fps);
}
static void sparse_sensor(int fps) {
    Settings settings;settings.source=kController;set_settings(settings);
    uint64_t stamp=5000000000;float zero[3]={},flat[3]={0,0,-1},right[3]={0,0,-float(pi/2)};
    controller_sample_gamecontroller(81,stamp,zero,flat);vpad(false);
    double angle=0;
    for(int tick=1;tick<=720;tick++) {
        if(tick%12==0)controller_sample_gamecontroller(81,stamp+uint64_t(std::llround(tick*1e9/720)),right,flat);
        if(tick%(720/fps)==0)angle+=-vpad(false).gyro.y*360/fps;
    }
    printf("60 Hz sensor at %d FPS: %.3f degrees/s integrated (physical 90)\n",fps,angle);
    assert(std::fabs(angle-90)<1.6);
    controller_sample_gamecontroller(81,stamp+1016666667,zero,flat);
    assert(near(vpad(false).gyro.length(),0));
    assert(near(vpad(false).gyro.length(),0));
    controller_sample_gamecontroller(81,stamp+1033333334,right,flat);
    controller_paused(81);auto paused=vpad(false);assert(near(paused.gyro.length(),0));
    controller_sample_gamecontroller(81,stamp+2033333334,right,flat);auto resumed=vpad(false);
    for(int i=0;i<3;i++)assert((paused.dir[i]-resumed.dir[i]).length()<1e-6); // no menu-duration catch-up
    controller_sample_gamecontroller(81,stamp+2050000001,right,flat);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));assert(near(vpad(false).gyro.length(),0));
    controller_gone(81);assert(near(vpad(false).gyro.length(),0));
}
// A shrine can take the controller around a closed compound rotation. Scaling each
// body rotation before integration makes this closed path stay open (even at equal X/Y).
static void shrine_then_bow(int fps) {
    uint64_t stamp=20000000000ull;constexpr int hz=240;
    float zero[3]={},flat[3]={0,0,-1};
    for(int variant=0;variant<4;variant++) {
        Settings s;s.source=kController;s.tuning.sensitivity_x=variant==1?1.f:.5f;
        s.tuning.sensitivity_y=variant==2?1.3f:.5f;
        s.tuning.invert_x=variant==3;s.tuning.invert_y=variant==3;
        set_settings(s);controller_sample_gamecontroller(91,stamp,zero,flat);
        auto initial=vpad(false);Matrix physical;
        const int axes[]={0,1,0,2};const double angles[]={pi/2,pi,-pi/2,pi};
        for(int round=0;round<3;round++) {
            for(int turn=0;turn<4;turn++)for(int frame=1;frame<=hz;frame++) {
                int axis=axes[turn];double rate=angles[turn];physical.rotate(axis,rate/hz);
                float gyro[3]={};gyro[axis==0?0:axis==1?2:1]=float(rate*(axis==1?-1:1));
                float accel[3]={float(physical.m[1][0]),float(physical.m[1][2]),float(-physical.m[1][1])};
                controller_sample_gamecontroller(91,stamp+=4166667,gyro,accel);
                if(frame*fps/hz!=(frame-1)*fps/hz)check_pose(initial,vpad(false),physical);
            }
            auto returned=vpad(false);check_pose(initial,returned,Matrix{});
            assert((returned.acc-initial.acc).length()<.001f);
            // Same bow-turn input before/after arbitrary shrine rounds retains its scale/sign.
            for(int axis=0;axis<2;axis++) {
                float gyro[3]={};gyro[axis==0?0:2]=axis==0?1.f:-1.f;
                controller_sample_gamecontroller(91,stamp+=4166667,gyro,flat);
                auto bow=vpad(false);
                double scale=axis==0?s.tuning.sensitivity_y:s.tuning.sensitivity_x;
                bool invert=axis==0?s.tuning.invert_y:s.tuning.invert_x;
                assert(near(component(bow.gyro,axis),-scale*(invert?-1:1)/(2*pi)));
                gyro[axis==0?0:2]*=-1;
                controller_sample_gamecontroller(91,stamp+=4166667,gyro,flat);vpad(false);
            }
        }
        controller_gone(91);
    }
    printf("PASS: shrine compound closed paths -> bow, custom sensitivity/invert, %d FPS\n",fps);
}
static void held_yaw_reference() {
    // Independent physical-world reference: the same turn about down must retain
    // the same speed with the controller flat, tilted or nearly upright.
    for(double pitch:{0.,30.,60.,85.})for(int sign:{-1,1}) {
        const double a=pitch*pi/180;Vec3 down{0,float(std::cos(a)),float(-std::sin(a))};
        VirtualPad pad;pad.observe_gravity(0,down);
        auto initial=pad.vpad();assert((initial.acc-Vec3{0,-down.y,down.z}).length()<1e-5);
        Aim physical{down.y*.01f,0,down.z*.01f};
        physical.yaw*=sign;physical.roll*=sign;
        Tuning tune;tune.sensitivity_x=tune.sensitivity_y=.5f;
        pad.turn_physical(.01f,physical,tune.apply(physical));pad.observe_gravity(.01f,down);
        auto motion=pad.vpad();
        Vec3 estimated_down{-motion.acc.x,-motion.acc.y,motion.acc.z};
        Vec3 rate{-motion.gyro.x,-motion.gyro.y,motion.gyro.z};
        assert(near(rate.dot(estimated_down),sign*.5/(2*pi),1e-5));
        // The gravity correction must not invent a turn or erase a real yaw heading.
        auto before=pad.orientation();pad.observe_gravity(.01f,down);auto after=pad.orientation();
        assert((before.rotate({1,0,0})-after.rotate({1,0,0})).length()<1e-5);
        assert(near(pad.vpad().gyro.length(),0));
        for(Vec3 bad:{Vec3{},Vec3{0,2,0},Vec3{NAN,0,0},Vec3{INFINITY,0,0}}) {
            pad.observe_gravity(.1f,bad);
            assert((before.rotate({1,0,0})-pad.orientation().rotate({1,0,0})).length()<1e-5);
        }
    }
    // Upside-down holds must converge without a zero cross-product singularity.
    VirtualPad upside;upside.observe_gravity(0,{0,-1,0});
    assert((upside.vpad().acc-Vec3{0,1,0}).length()<1e-5);
    puts("PASS: left/right world yaw at 0/30/60/85-degree holds; invalid/shaken/upside-down gravity; no synthetic rate");
}
static void gravity_recovery(int fps) {
    Settings s;s.source=kController;s.tuning.sensitivity_x=s.tuning.sensitivity_y=.5f;set_settings(s);
    uint64_t stamp=100000000000ull;constexpr int hz=240;
    float zero[3]={},flat[3]={0,0,-1};
    // No sensor samples arrive while the settings window is open. The physical pad
    // changes pitch, but the game must not replay those missing gyro samples.
    controller_sample_gamecontroller(101,stamp,zero,flat);vpad(false);
    controller_paused(101);auto paused=vpad(false);
    float upright[3]={0,-1,0};
    controller_sample_gamecontroller(101,stamp+=1000000000,zero,upright);
    auto resumed=vpad(false);for(int i=0;i<3;i++)assert((paused.dir[i]-resumed.dir[i]).length()<1e-6);
    for(int i=0;i<8*hz;i++) {
        controller_sample_gamecontroller(101,stamp+=4166667,zero,upright);
        if(i*fps/hz!=(i+1)*fps/hz)assert(near(vpad(false).gyro.length(),0));
    }
    auto settled=vpad(false);
    printf("post-menu gravity: %.5f %.5f %.5f (expected 0 0 -1)\n",settled.acc.x,settled.acc.y,settled.acc.z);fflush(stdout);
    assert((settled.acc-Vec3{0,0,-1}).length()<.01f);
    // Two minutes of small pitch bias while turning left/right prevents a stationary
    // bias estimate. Acceleration continues to identify down, so tilt must stay bounded.
    controller_gone(101);controller_sample_gamecontroller(102,stamp+=1000000000,zero,flat);
    for(int i=0;i<8*hz;i++)controller_sample_gamecontroller(102,stamp+=4166667,zero,flat);
    vpad(false);
    for(int i=0;i<120*hz;i++) {
        float gyro[3]={.02f,0,(i/hz)%2?.4f:-.4f};
        controller_sample_gamecontroller(102,stamp+=4166667,gyro,flat);
        if(i*fps/hz!=(i+1)*fps/hz)vpad(false);
    }
    auto drift=vpad(false);
    printf("two-minute gravity: %.5f %.5f %.5f (expected near 0 -1 0)\n",drift.acc.x,drift.acc.y,drift.acc.z);fflush(stdout);
    assert((drift.acc-Vec3{0,-1,0}).length()<.04f);
    for(int direction:{-1,1}) {
        float yaw[3]={0,0,float(-direction)};
        controller_sample_gamecontroller(102,stamp+=4166667,yaw,flat);
        auto bow=vpad(false);
        Vec3 down{-bow.acc.x,-bow.acc.y,bow.acc.z};
        Vec3 rate{-bow.gyro.x,-bow.gyro.y,bow.gyro.z};
        assert(near(rate.dot(down),direction*.5/(2*pi),.001));
    }
    puts("PASS: left/right retain sign and speed after two-minute drift/recovery sequence");
    controller_gone(102);
}
int main(int argc,char** argv) {held_yaw_reference();shrine_then_bow(argc>1?std::atoi(argv[1]):60);migration();golden_axes();production(argc>1?std::atoi(argv[1]):60);sparse_sensor(argc>1?std::atoi(argv[1]):60);gravity_recovery(argc>1?std::atoi(argv[1]):60);}
