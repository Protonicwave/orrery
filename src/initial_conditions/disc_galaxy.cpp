#include "orrery/initial_conditions/disc_galaxy.hpp"

#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/initial_conditions/centre_of_mass_frame.hpp"
#include "orrery/initial_conditions/plummer.hpp"

namespace orrery::initial_conditions {

namespace {

using core::Index;
using core::kGravitationalConstant;
using core::Real;
using core::Vec3;

void require_valid_model(const DiscGalaxyParameters& parameters) {
    // Each comparison is the condition wanted, negated, so that a NaN is
    // rejected rather than slipping through a `<= 0` that is false for it. The
    // Plummer sampler states the same reasoning at greater length.
    if (!(parameters.disc_mass > 0)) {
        throw std::invalid_argument{"A disc galaxy needs a positive disc mass"};
    }
    if (!(parameters.bulge_mass >= 0)) {
        throw std::invalid_argument{"A disc galaxy cannot have a negative bulge mass"};
    }
    if (!(parameters.scale_length > 0)) {
        throw std::invalid_argument{"A disc galaxy needs a positive scale length"};
    }
    if (!(parameters.scale_height > 0)) {
        throw std::invalid_argument{"A disc galaxy needs a positive scale height"};
    }
    if (!(parameters.bulge_radius > 0)) {
        // Required even for a galaxy with no bulge, because it is the scale
        // radius of a Plummer sphere and a Plummer sphere of zero scale radius
        // is a point mass with an infinite central potential. A caller that
        // wants no bulge sets the mass to zero and leaves this alone.
        throw std::invalid_argument{"A disc galaxy needs a positive bulge radius"};
    }
    if (!(parameters.softening >= 0)) {
        throw std::invalid_argument{"A disc galaxy cannot have a negative softening length"};
    }
    if (!(parameters.mass_fraction_cutoff > 0) || !(parameters.mass_fraction_cutoff < 1)) {
        throw std::invalid_argument{"A disc galaxy needs a mass fraction cutoff in (0, 1)"};
    }
}

/// The fraction of an untruncated exponential disc's mass inside `x` scale
/// lengths.
///
/// `1 - (1 + x) exp(-x)`, the integral of `2 pi R Sigma_0 exp(-R / R_d)`. It has
/// no closed-form inverse, which is why the radii below are drawn by bisection
/// rather than by the direct inversion the Plummer sphere allows.
[[nodiscard]] Real enclosed_fraction(Real x) {
    return 1 - ((1 + x) * std::exp(-x));
}

/// The radius, in scale lengths, enclosing `fraction` of the untruncated disc.
///
/// Bisection rather than Newton's method. The derivative is available and
/// Newton would converge in a handful of steps rather than fifty, but this runs
/// once per particle at setup and never inside a step, and bisection cannot fail
/// to converge or leave the bracket. A setup routine that occasionally returned
/// a wrong radius for an unlucky draw would be a hard thing to notice and a
/// harder thing to reproduce.
[[nodiscard]] Real radius_enclosing(Real fraction) {
    Real low = 0;
    Real high = 1;
    while (enclosed_fraction(high) < fraction) {
        high *= 2;
    }

    // Stops when the bracket is at the resolution of the type rather than after
    // a fixed count, so that the single-precision build does not spend thirty
    // iterations halving an interval it can no longer represent.
    const Real tolerance = std::numeric_limits<Real>::epsilon();
    while ((high - low) > tolerance * (1 + high)) {
        const Real middle = (low + high) / 2;
        if (enclosed_fraction(middle) < fraction) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return (low + high) / 2;
}

/// The mass of a Plummer sphere inside radius `radius`.
[[nodiscard]] Real plummer_enclosed_mass(Real mass, Real scale_radius, Real radius) {
    const Real ratio = radius / std::sqrt((radius * radius) + (scale_radius * scale_radius));
    return mass * ratio * ratio * ratio;
}

/// The z axis carried through the inclination and then the position angle.
///
/// One function for both the axis and the particles, so that a galaxy's spin
/// axis is by construction the axis its particles were rotated about rather than
/// a second derivation of the same thing.
[[nodiscard]] Vec3 rotate(Vec3 v, Real inclination, Real position_angle) {
    const Real cos_i = std::cos(inclination);
    const Real sin_i = std::sin(inclination);
    const Real cos_a = std::cos(position_angle);
    const Real sin_a = std::sin(position_angle);

    // A tilt about the x axis, then a rotation about z. Written out rather than
    // built from a matrix type, because this file is the only place in the
    // project that needs a rotation and a 3-by-3 matrix that exists for one
    // caller is a type nobody maintains.
    const Real y = (v.y * cos_i) - (v.z * sin_i);
    const Real z = (v.y * sin_i) + (v.z * cos_i);

    return {(v.x * cos_a) - (y * sin_a), (v.x * sin_a) + (y * cos_a), z};
}

/// The realised component masses, which are the requested ones rounded to whole
/// particles.
struct Components {
    Index disc_count = 0;
    Index bulge_count = 0;
    Real particle_mass = 0;
    Real disc_mass = 0;
    Real bulge_mass = 0;
};

[[nodiscard]] Components components_of(const DiscGalaxyParameters& parameters) {
    Components components;
    if (parameters.count == 0) {
        return components;
    }

    components.disc_count = disc_galaxy_disc_count(parameters);
    components.bulge_count = parameters.count - components.disc_count;
    components.particle_mass =
        (parameters.disc_mass + parameters.bulge_mass) / static_cast<Real>(parameters.count);
    components.disc_mass = static_cast<Real>(components.disc_count) * components.particle_mass;
    components.bulge_mass = static_cast<Real>(components.bulge_count) * components.particle_mass;
    return components;
}

/// Put the first `disc_count` particles on circular orbits about the spin axis.
///
/// Separate from the placement above because it has to run after the sample has
/// been centred: the radius that decides a particle's speed is its distance from
/// the galaxy's centre, and where that centre is depends on every particle.
void set_disc_velocities(core::ParticleData& data, const DiscGalaxyParameters& parameters,
                         const Components& components) {
    const Vec3 axis = disc_galaxy_spin_axis(parameters);
    const core::Vec3Span<const Real> positions = data.positions();
    const core::Vec3Span<Real> velocities = data.velocities();

    for (Index particle = 0; particle < components.disc_count; ++particle) {
        const Vec3 position = positions.get(particle);

        // The component of the position perpendicular to the spin axis. That,
        // rather than the distance from the origin, is the radius of the circle
        // the particle travels on, and the two differ by the disc's thickness.
        const Vec3 offset = position - (axis * core::dot(position, axis));
        const Real radius = core::norm(offset);
        if (!(radius > 0)) {
            // A particle exactly on the axis has no direction to circle in. It
            // is left at rest, which is what a zero circular speed means anyway.
            continue;
        }

        // The direction of travel is perpendicular to both the axis and the
        // radius, which is what makes the orbit circular rather than merely fast
        // enough. The cross product of two perpendicular vectors of known length
        // needs no normalisation beyond dividing by the radius.
        const Vec3 direction = core::cross(axis, offset) / radius;
        velocities.set(particle, direction * disc_galaxy_circular_speed(parameters, radius));
    }
}

} // namespace

Index disc_galaxy_disc_count(const DiscGalaxyParameters& parameters) {
    require_valid_model(parameters);
    if (parameters.count == 0) {
        return 0;
    }

    const Real fraction = parameters.disc_mass / (parameters.disc_mass + parameters.bulge_mass);
    const auto rounded = static_cast<Index>(
        std::llround(static_cast<double>(fraction) * static_cast<double>(parameters.count)));

    // At least one and no more than the count. The lower clamp matters for a
    // galaxy of very few particles with a heavy bulge, where the rounded share
    // can be zero: a configuration named for its disc should not be able to
    // produce one with no disc in it.
    if (rounded == 0) {
        return 1;
    }
    return rounded > parameters.count ? parameters.count : rounded;
}

Real disc_galaxy_particle_mass(const DiscGalaxyParameters& parameters) {
    return components_of(parameters).particle_mass;
}

Real disc_galaxy_total_mass(const DiscGalaxyParameters& parameters) {
    const Components components = components_of(parameters);
    return components.disc_mass + components.bulge_mass;
}

Real disc_galaxy_enclosed_mass(const DiscGalaxyParameters& parameters, Real radius) {
    const Components components = components_of(parameters);
    if (radius <= 0) {
        return 0;
    }

    // The disc's share is measured against the truncated sample rather than
    // against the infinite model, so that the enclosed mass reaches the whole of
    // the disc at the cutoff radius rather than one per cent short of it. The
    // sample holds `mass_fraction_cutoff` of the model, which is what the
    // division by it says.
    const Real fraction =
        enclosed_fraction(radius / parameters.scale_length) / parameters.mass_fraction_cutoff;
    const Real disc = components.disc_mass * (fraction < 1 ? fraction : 1);

    return disc + plummer_enclosed_mass(components.bulge_mass, parameters.bulge_radius, radius);
}

Real disc_galaxy_circular_speed(const DiscGalaxyParameters& parameters, Real radius) {
    if (radius <= 0) {
        return 0;
    }

    // The acceleration of a Plummer-softened point mass times the radius. The
    // softening appears here and not only in the solver because a disc built
    // for one force law and integrated under another starts out of balance,
    // which is the whole of the note in the header.
    const Real softening = parameters.softening;
    const Real softened = std::sqrt((radius * radius) + (softening * softening));

    return std::sqrt(kGravitationalConstant * disc_galaxy_enclosed_mass(parameters, radius) *
                     radius * radius / (softened * softened * softened));
}

Vec3 disc_galaxy_spin_axis(const DiscGalaxyParameters& parameters) {
    require_valid_model(parameters);
    return rotate({0, 0, 1}, parameters.inclination, parameters.position_angle);
}

core::ParticleData make_disc_galaxy(const DiscGalaxyParameters& parameters,
                                    core::RandomSource& random) {
    require_valid_model(parameters);
    if (parameters.count == 0) {
        return {};
    }

    const Components components = components_of(parameters);
    const Real cutoff = parameters.mass_fraction_cutoff;

    core::ParticleData data;
    data.reserve(parameters.count);

    // The disc is placed first and left at rest, because the speed a particle
    // needs depends on its distance from the centre of the galaxy and the centre
    // of the galaxy is not known until every particle has been drawn. Assigning
    // velocities from the radii of an uncentred sample and then shifting it
    // would leave each particle circling a point a fraction of a scale length
    // away from the one it was given a speed for.
    for (Index particle = 0; particle < components.disc_count; ++particle) {
        // Drawn from the enclosed mass fraction rather than from the radius, so
        // the sample follows the surface density rather than piling up in the
        // outskirts where the annuli are larger.
        const Real radius = parameters.scale_length * radius_enclosing(random.uniform(0, cutoff));
        const Real angle = random.uniform(0, 2 * std::numbers::pi_v<Real>);

        // An exponential vertical profile, drawn by inverting its cumulative
        // distribution. The draw is from [0, 1), so the logarithm is of a value
        // in (0, 1] and cannot be of zero.
        const Real height = -parameters.scale_height * std::log(1 - random.uniform()) *
                            (random.uniform() < static_cast<Real>(0.5) ? Real{-1} : Real{1});

        const Vec3 position{radius * std::cos(angle), radius * std::sin(angle), height};
        data.add(rotate(position, parameters.inclination, parameters.position_angle), Vec3{},
                 components.particle_mass);
    }

    if (components.bulge_count > 0 && components.bulge_mass > 0) {
        // The bulge is the sampler in plummer.hpp rather than a second copy of
        // it. Its own cutoff is left at that sampler's default: the disc's
        // cutoff is a property of the disc, and a bulge truncated at the same
        // ninety-nine per cent would be a different model from the one the
        // enclosed mass above assumes.
        const PlummerParameters bulge{.count = components.bulge_count,
                                      .total_mass = components.bulge_mass,
                                      .scale_radius = parameters.bulge_radius};

        const core::ParticleData sphere = make_plummer_sphere(bulge, random);
        const core::Vec3Span<const Real> positions = sphere.positions();
        const core::Vec3Span<const Real> velocities = sphere.velocities();

        // Appended unrotated. The bulge is spherical and its velocities are
        // isotropic, so rotating it would move every particle and change
        // nothing about the configuration.
        for (Index particle = 0; particle < sphere.size(); ++particle) {
            data.add(positions.get(particle), velocities.get(particle), components.particle_mass);
        }
    }

    // Centre the sample, which is what the disc's velocities are about to be
    // measured from. The velocity half of this call has only the bulge to act
    // on and removes the net momentum of its draw, which is the right thing to
    // do to it and is why there is no position-only version of this function.
    move_to_centre_of_mass_frame(data);
    set_disc_velocities(data, parameters, components);

    // The disc's rotation is a sum of N draws around a circle and so carries a
    // net momentum of order one part in the square root of the count. Removing
    // it is a Galilean boost: it changes no separation, no relative velocity and
    // no energy, and it leaves the conserved momentum this run has to hold
    // constant at exactly zero rather than at a small number.
    move_to_centre_of_mass_frame(data);

    return data;
}

} // namespace orrery::initial_conditions
