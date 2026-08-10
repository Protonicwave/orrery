#include "orrery/integrators/yoshida4.hpp"

#include <limits>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/integrators/acceleration_field.hpp"
#include "orrery/integrators/velocity_verlet.hpp"

namespace orrery::integrators {

namespace {

using core::Real;

/// The cube root of two, from which both weights follow.
///
/// Written out rather than called for. `std::cbrt` is not usable in a constant
/// expression in C++20, so computing it here would put a function call into the
/// initialisation of a namespace-scope object, and this way the value is a
/// compile-time constant in both of the precisions the project builds in. The
/// digits are enough to round correctly to `double`, which has fifteen, and to
/// `float`, which has seven.
constexpr Real kCubeRootOfTwo = static_cast<Real>(1.2599210498948731647672106);

/// `w1`, applied to the first and third substeps.
constexpr Real kOuterWeight = Real{1} / (2 - kCubeRootOfTwo);

/// `w0`, applied to the second, and negative. See the header.
constexpr Real kInnerWeight = -kCubeRootOfTwo / (2 - kCubeRootOfTwo);

/// The three substeps advance the state by `h` in total.
///
/// To rounding rather than exactly: the two weights are separately rounded
/// quotients of the same denominator, and their sum need not land on one. A slip
/// in either would still integrate something, at second order and over the wrong
/// interval, which is the kind of error a convergence measurement alone might not
/// localise.
constexpr Real kNetAdvance = (2 * kOuterWeight) + kInnerWeight;
static_assert(kNetAdvance > 1 - (4 * std::numeric_limits<Real>::epsilon()));
static_assert(kNetAdvance < 1 + (4 * std::numeric_limits<Real>::epsilon()));

} // namespace

void Yoshida4::step(core::ParticleData& data, Real timestep, AccelerationField& field) {
    // Each substep is a complete velocity Verlet step, so each requires and
    // restores the acceleration invariant, and the composition therefore does
    // too. Three substeps, three force evaluations.
    //
    // The half-kick that ends one substep and the half-kick that begins the next
    // act on the same accelerations at the same positions and could be merged
    // into one. That would save six of the twenty-seven vector loops in a step and
    // would cost the property that makes this file short, which is that the
    // method is visibly a composition rather than a transcription. Against three
    // force evaluations of N^2 interactions the saving is not measurable.
    velocity_verlet_step(data, kOuterWeight * timestep, field);
    velocity_verlet_step(data, kInnerWeight * timestep, field);
    velocity_verlet_step(data, kOuterWeight * timestep, field);
}

} // namespace orrery::integrators
