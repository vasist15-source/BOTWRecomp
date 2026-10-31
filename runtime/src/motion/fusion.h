// Motion sensor math for the virtual GamePad gyro (see motion.h): axis mapping of the host sources, gyro
// bias estimation, the gravity direction, the aim conventions (player space, yaw, roll) and the virtual
// GamePad whose direction matrix VPADRead reports.
//
// Plain C++ (no SDL, no sockets), unit-tested in runtime/tools/motion_test.cpp.
//
// BotW needs all three axes: shrines consume orientation/tilt as well as angular rate.
// Legacy two-axis modes are retained for players who explicitly choose an aiming-only mapping.
// The default native mode integrates unscaled physical body turns at sensor cadence; VPAD
// gyro/angle apply the aiming sensitivity independently. Neither FPS nor sensitivity may
// distort the physical orientation after a closed sequence of shrine turns.
//
// Frames. Adapted from Cemu (MPL-2.0, https://github.com/cemu-project/Cemu): the axis signs of the SDL and
// DSU sources and the VPAD layout; the code here is our own.
//   - Host frame "H": the SDL controller frame turned by 180 degrees about X: x right, y down out of the
//     face, z away from the player. acc_H is the gravity direction in g (at rest flat on a table:
//     (0, 1, 0)); gyro_H is in radians per second. In H, turning a flat controller to the right is +y,
//     tilting its top up is +x and rolling it to the right (right side down) is +z.
//   - GamePad frame (VPAD): x = H.x, y = H.y, z = -H.z (a mirror of H, as the hardware reports). VPAD gyro is
//     in revolutions per second (1.0 = 360 deg/s), angle in revolutions, acc in g (specific force), dir = the
//     GamePad's X, Y and Z axes in world coordinates (x, z, y swizzle), the identity when it lies flat with
//     its top away from the player.
#pragma once
#include <cmath>
#include <cstdint>

namespace motion {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(Vec3 o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    float length() const { return std::sqrt(dot(*this)); }
};

// unit quaternion, Hamilton product; rotates body (H) vectors into the world frame
struct Quat {
    float w = 1, x = 0, y = 0, z = 0;
    Quat() = default;
    Quat(float a, float b, float c, float d) : w(a), x(b), y(c), z(d) {}
    static Quat axis_angle(Vec3 axis, float radians);
    Quat operator*(const Quat& r) const {
        return {w * r.w - x * r.x - y * r.y - z * r.z, w * r.x + x * r.w + y * r.z - z * r.y,
                w * r.y - x * r.z + y * r.w + z * r.x, w * r.z + x * r.y - y * r.x + z * r.w};
    }
    Quat conj() const { return {w, -x, -y, -z}; }
    void normalize();
    Vec3 rotate(Vec3 v) const;      // body -> world
    Vec3 unrotate(Vec3 v) const;    // world -> body
};

// The values VPADRead reports (VPADStatus +0x1C..+0x8F).
struct VpadMotion {
    Vec3 acc{0, -1, 0};          // +0x1C, g (flat on a table: gravity along -y)
    float acc_magnitude = 1;     // +0x28
    float acc_variation = 0;     // +0x2C, change of acc since the previous sample
    float acc_xy[2] = {1, 0};    // +0x30
    Vec3 gyro;                   // +0x38, revolutions per second
    Vec3 angle;                  // +0x44, revolutions (gyro integrated)
    Vec3 dir[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};  // +0x6C, +0x78, +0x84
};

// ---- host sources -> frame H ----
// SDL: gyro rad/s and acceleration m/s^2 in SDL's frame (x right, y up, z towards the player)
void from_sdl(const float gyro[3], const float accel[3], Vec3& gyro_h, Vec3& acc_h);
// Apple GameController: rad/s, gravity-inclusive acceleration in g, x right / y top / z face-up.
void from_gamecontroller(const float gyro[3], const float accel[3], Vec3& gyro_h, Vec3& acc_h);
// Cemuhook (DSU): gyro deg/s (pitch, yaw, roll) and acceleration in g, as DS4Windows and the others send
void from_dsu(const float gyro_deg[3], const float accel_g[3], Vec3& gyro_h, Vec3& acc_h);

// Which motion of the controller turns the view left/right (the usual gyro aiming conventions, as in
// JoyShockMapper and Steam Input). Up/down is always tilting the controller's top up or down.
enum AxisMode : int {
    kPlayerSpace,  // aiming-only: turn about the real vertical, however the controller is held
    kYawAxis,      // turning about the controller's own vertical axis (best held flat)
    kRollAxis,     // rolling it like a steering wheel
    kNativeAxes,   // all three physical axes, for BotW aiming and motion-controlled shrine apparatus
    kAxisModeCount
};
const char* axis_id(int a);     // "player", "yaw", "roll", "native"
const char* axis_label(int a);
int axis_from_id(const char* id);  // -1 if unknown

// Physical/aim turn in radians: yaw right, pitch up, roll right side down.
struct Aim {
    float yaw = 0, pitch = 0, roll = 0;
    Aim operator+(Aim o) const { return {yaw + o.yaw, pitch + o.pitch, roll + o.roll}; }
};

// Sensitivity is the virtual GamePad turn per physical turn. BotW defaults to full three-axis
// motion at 1x; the legacy WWHD aiming conventions remain opt-in. A shrine needs actual roll,
// not roll projected onto a two-dimensional aiming direction.
struct Tuning {
    static constexpr float kDefaultSensitivity = 1.0f, kMinSensitivity = 0.05f, kMaxSensitivity = 5.0f;
    float sensitivity_x = kDefaultSensitivity, sensitivity_y = kDefaultSensitivity;  // left/right, up/down
    bool invert_x = false, invert_y = false;
    int axis = kNativeAxes;  // AxisMode (controller sources; the mouse already moves left/right and up/down)
    bool operator==(const Tuning&) const = default;
    Aim apply(Aim a) const {
        return {a.yaw * sensitivity_x * (invert_x ? -1.0f : 1.0f), a.pitch * sensitivity_y * (invert_y ? -1.0f : 1.0f),
                a.roll * sensitivity_x * (invert_x ? -1.0f : 1.0f)};
    }
};

// One motion sensor (a controller or the Cemuhook slot): gyro bias estimation while the controller rests
// (no slow drift), a noise floor, the gravity direction (accelerometer, carried along by the gyro) and the
// aim it reads from the motion under an AxisMode.
class Fusion {
public:
    void recalibrate();    // learn the gyro bias anew the next time the controller rests
    // one sensor sample: dt seconds since the previous one, gyro_h rad/s, acc_h in g (zero vector: no
    // accelerometer); the aim of this sample adds to what take() returns
    void update(float dt, Vec3 gyro_h, Vec3 acc_h, int axis_mode);
    Aim take();            // the aim since the previous take() (radians), and starts anew
    Vec3 bias() const { return bias_; }
    Vec3 gravity() const { return grav_; }   // gravity direction in frame H (unit)
    float rate() const { return rate_; }     // the latest bias-free turning rate (rad/s)
    bool calibrated() const { return bias_samples_ >= kBiasSamples; }
    // the gyro reads zero below this rate (rad/s, after bias removal): sensor noise
    static constexpr float kNoise = 0.008f;
    static constexpr int kBiasSamples = 100;
    // the aim rate (rad/s right / up) of a bias-free rate w (rad/s, frame H) with gravity direction g
    // (frame H, unit)
    static Aim aim_rate(Vec3 w, Vec3 g, int axis_mode);

private:
    Vec3 bias_;
    int bias_samples_ = 0;       // samples in the running bias average (capped: it follows slow changes)
    float still_time_ = 0;       // seconds the controller has rested
    Vec3 last_acc_h_, last_gyro_h_;
    Vec3 grav_{0, 1, 0};
    bool have_grav_ = false;     // grav_ has been set from the accelerometer
    float rate_ = 0;
    Aim aim_;
};

// The virtual GamePad integrates the ordered three-axis sensor turns, then reports VPAD values.
class VirtualPad {
public:
    VirtualPad();
    void turn(float dt, Aim a);
    // Native three-axis pose follows the physical controller, regardless of aiming gain.
    // Only angular rate/angle use the tuned response; scaling pose increments makes a
    // closed compound rotation fail to return to its starting orientation.
    void turn_physical(float dt, Aim physical, Aim response);
    // Native sensors provide an absolute down direction. Correct pitch/roll drift
    // without changing yaw or synthesizing gyro input. Ignore shaking/invalid data.
    void observe_gravity(float dt, Vec3 acceleration);
    // Sensor sources retain the latest rate between packets (e.g. 60 Hz sensor / 120 FPS game).
    // The caller expires stale sources; mouse/test turns use one-shot rates.
    VpadMotion vpad(bool hold_rate = false);
    Quat orientation() const { return q_; }
    void stop_rate() { window_ = 0; angle_read_ = angle_; last_rate_v_ = {}; }

private:
    Quat q_;
    bool have_gravity_ = false;
    Vec3 angle_, angle_read_, last_rate_v_;  // revolutions, VPAD frame
    double window_ = 0;                      // seconds turned since the last vpad()
};

}  // namespace motion
