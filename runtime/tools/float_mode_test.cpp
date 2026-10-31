#include <cmath>
#include "ppc.h"
#include <cassert>
#include <cstdio>
#include <limits>

// The camera's reciprocal-square-root refinement over a tiny positive
// squared length. NI must remove the underflow before this branch executes.
static double refined_length(Cpu* c, double x, bool legacy) {
    auto single = [=](double v) { return legacy ? to_single(v) : ppc_to_single(c, v); };
    const double squared = single(x * round25(x));
    if (squared == 0.0) return squared;
    const double estimate = ppc_frsqrte(squared);
    const double estimate_squared = single(estimate * round25(estimate));
    const double correction = single(3.0 - estimate_squared * round25(squared));
    const double half_estimate = single(estimate * 0.5);
    return single(squared * round25(single(correction * round25(half_estimate))));
}

int main() {
    Cpu cpu{};
    const double minimum = std::numeric_limits<float>::min();
    const double tiny = std::numeric_limits<float>::denorm_min();
    assert(ppc_to_single(&cpu, tiny) == tiny);
    cpu.fpscr = 0x8004; // captured camera thread: FPSCR.NI set
    assert(ppc_to_single(&cpu, tiny) == 0.0);
    assert(std::signbit(ppc_to_single(&cpu, -tiny)));
    assert(ppc_to_single(&cpu, minimum) == minimum);
    const double below = std::nextafter(minimum, 0.0);
    assert(to_single(below) == minimum);
    assert(ppc_to_single(&cpu, below) == 0.0);
    assert(ppc_to_single(&cpu, 1.25) == 1.25);
    assert(std::isinf(ppc_to_single(&cpu, INFINITY)));
    assert(std::isnan(ppc_to_single(&cpu, NAN)));
    // Magnitude captured at camera +0x7e0 before its first invalid position.
    const double camera_drift = 9.12504817803691e-22;
    const double old_length = refined_length(&cpu, camera_drift, true);
    assert(!std::isfinite(old_length));
    assert(refined_length(&cpu, camera_drift, false) == 0.0);
    assert(std::abs(refined_length(&cpu, 2.0, false) - 2.0) < 1e-5);
    cpu.fpscr = 0;
    assert(ppc_to_single(&cpu, tiny) == tiny);
    std::printf("NI underflow regression: legacy length=%g, NI length=0; boundaries passed\n", old_length);
}
