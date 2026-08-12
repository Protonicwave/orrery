#pragma once

/// \file
/// Two disc galaxies on a collision course: the project's headline scenario.
///
/// Section 7 of the implementation plan names the galaxy collision as the
/// demonstration Phase 12 exists to produce, and this is where it is defined
/// rather than in the renderer that draws it. That separation is the point: the
/// scenario is a configuration of point masses like any other, it can be
/// integrated with any solver in the project, its energy is conserved to the
/// same tolerance as anything else, and the renderer has no opinion about it.
/// A demonstration that only existed inside the viewer would be an animation
/// rather than a result.
///
/// ## The encounter
///
/// Each galaxy is placed at rest in its own centre-of-mass frame by
/// `disc_galaxy.hpp`, and then the two are put on the two-body orbit their total
/// masses would follow if each were a point. The separation vector runs along
/// the x axis with the impact parameter offset along y, and the relative
/// velocity is directed along the separation's own x component, so the encounter
/// is planar in x-y whatever the discs are inclined to.
///
/// The relative speed is a multiple of the local escape speed,
/// `sqrt(2 G M / d)`, where `M` is the total mass of both galaxies and `d` the
/// separation. That parametrisation is worth the small amount of arithmetic it
/// costs, because the multiplier says what kind of encounter it is without any
/// further calculation: below one the pair is bound and will merge, at one the
/// orbit is exactly parabolic, and above one the galaxies pass once and separate
/// for ever. The parabolic case is also an analytic check, since the two-body
/// energy of the encounter is then exactly zero, and the tests use it as one.
///
/// A merger is the interesting case and the default. Real major mergers are
/// bound encounters, tidal tails come from the first passage, and the second
/// passage is what actually destroys the discs.
///
/// ## What is being approximated
///
/// The placement treats each galaxy as a point mass, which is exact only while
/// the separation is large compared with the galaxies. At the default separation
/// of twenty disc scale lengths the leading correction is the quadrupole of each
/// disc, and it is small. It is not zero, so the encounter that actually happens
/// is not precisely the Kepler orbit it was placed on, and nothing in the tests
/// claims otherwise: what they check is the state at the moment it was built.

#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/disc_galaxy.hpp"

namespace orrery::initial_conditions {

/// An encounter between two disc galaxies.
struct GalaxyCollisionParameters {
    /// The galaxy that comes first in the particle array.
    ///
    /// Its `count` is its own, not a share of a total, so a minor merger is
    /// described by giving the secondary fewer particles as well as less mass.
    /// Equal particle masses across both galaxies are the caller's business
    /// here: it is a reasonable thing to want and a reasonable thing not to,
    /// since a secondary sampled at the same mass resolution as the primary
    /// costs particles in proportion to its mass and resolves its tidal tail
    /// far better.
    DiscGalaxyParameters primary;

    DiscGalaxyParameters secondary;

    /// The x component of the separation between the two centres.
    core::Real separation = 20;

    /// The y component of the separation, which is what makes the encounter
    /// grazing rather than head on.
    ///
    /// A head-on collision, with this zero, produces a shell rather than the
    /// tidal tails the demonstration is for: the tails are drawn out by the
    /// angular momentum of the orbit, and an encounter with none has none to
    /// give. Two disc scale lengths is enough to produce them and small enough
    /// that the discs still meet.
    core::Real impact_parameter = 2;

    /// The relative speed, as a multiple of the escape speed at the initial
    /// separation.
    ///
    /// One is parabolic. Below one is bound and merges; above one is hyperbolic
    /// and does not. Eight tenths gives a pair that falls together, passes,
    /// separates to a few times the initial separation and returns, which is the
    /// sequence a real merger follows and is what the demonstration runs for.
    core::Real approach_speed = static_cast<core::Real>(0.8);
};

/// Sample both galaxies and place them on the encounter, in the combined
/// centre-of-mass frame.
///
/// The primary's particles come first, then the secondary's, so a renderer can
/// tint the two apart by index alone and the tests can measure either galaxy's
/// centre of mass on its own. Both galaxies are drawn from the same random
/// stream, in that order.
///
/// Throws `std::invalid_argument` for anything either galaxy refuses, and for a
/// separation vector of zero length, which has no direction to approach along.
/// A negative approach speed is refused rather than read as a receding pair:
/// two galaxies moving apart is a configuration, but it is not a collision, and
/// a caller that meant it can write it with a negative separation instead.
[[nodiscard]] core::ParticleData make_galaxy_collision(const GalaxyCollisionParameters& parameters,
                                                       core::RandomSource& random);

/// The separation vector from the primary's centre to the secondary's.
///
/// `(separation, impact_parameter, 0)`. Exposed so that a test can measure the
/// sampled configuration against the vector it was built from rather than
/// against a second spelling of it.
[[nodiscard]] core::Vec3 galaxy_collision_separation(const GalaxyCollisionParameters& parameters);

/// The relative velocity of the secondary with respect to the primary.
///
/// Directed along the negative x axis, with magnitude `approach_speed` times
/// `sqrt(2 G M / d)`.
[[nodiscard]] core::Vec3
galaxy_collision_relative_velocity(const GalaxyCollisionParameters& parameters);

/// The energy of the encounter, treating each galaxy as a point mass.
///
/// `mu v^2 / 2 - G M_1 M_2 / d`. Negative for a bound pair, zero for the
/// parabolic case and positive for a hyperbolic one, which is the statement the
/// approach speed parameter makes and the one the tests check it against.
[[nodiscard]] core::Real galaxy_collision_orbit_energy(const GalaxyCollisionParameters& parameters);

} // namespace orrery::initial_conditions
