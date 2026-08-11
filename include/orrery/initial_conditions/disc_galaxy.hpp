#pragma once

/// \file
/// A rotating disc galaxy: the configuration the project's demonstration is
/// built from.
///
/// Every configuration before this one is spherical and either in equilibrium or
/// at rest, which is what a conservation test wants and what a picture does not.
/// A galaxy is the opposite case: nearly all of its kinetic energy is in ordered
/// rotation rather than in random motion, so it has a shape that persists, an
/// axis that means something, and structure that develops as it turns. That is
/// what makes it worth looking at, and Phase 12 of the implementation plan is
/// the phase that needs something worth looking at.
///
/// The model is a Plummer bulge with an exponential disc around it, which is the
/// standard two-component decomposition of a disc galaxy and the simplest one
/// whose parts both have closed forms. The bulge is drawn by the sampler in
/// `plummer.hpp` rather than by a second copy of it here. The disc is drawn from
/// the surface density `Sigma(R) = Sigma_0 exp(-R / R_d)` with an exponential
/// vertical profile, and every disc particle is placed on the circular orbit
/// that the enclosed mass at its radius supports.
///
/// ## What this model is, and what it is not
///
/// It is not an equilibrium solution. A Plummer sphere is: its density and its
/// distribution of velocities solve the collisionless Boltzmann equation
/// together, so a sample from it stays as it was drawn, and the conservation
/// tests can hold it to that. This does not, in three separate ways, each of
/// which is a deliberate simplification rather than an oversight.
///
/// **The disc's own gravity is treated as spherical** when the circular speed is
/// computed. The true circular speed of a razor-thin exponential disc involves
/// Bessel functions of the enclosed surface density and is about fifteen per
/// cent higher near the peak of the rotation curve than the spherical
/// approximation gives. Implementing it would add a special-function evaluation
/// per particle to a setup routine and change nothing a viewer would notice,
/// since the disc is not in equilibrium anyway.
///
/// **The bulge is drawn as though the disc were not there.** Its velocities come
/// from the distribution function of an isolated Plummer sphere, so it is
/// slightly sub-virial in the deeper combined potential and contracts a little
/// over the first dynamical time.
///
/// **The disc is cold.** Particles get the circular speed exactly, with no
/// velocity dispersion, so the Toomre stability parameter is zero everywhere and
/// the disc is violently unstable to its own self-gravity. That is not a defect
/// to be apologised for: it is why the model develops flocculent spiral arms and
/// a bar within a few rotations, which is the behaviour the demonstration is
/// there to show. A disc heated enough to be stable would be a smooth disc that
/// stays a smooth disc.
///
/// A configuration that needs equilibrium should use the Plummer sphere, which
/// has it and is validated against it. This one is a scenario, and what it is
/// validated against is what it claims: the particle count, equal masses, a
/// centre of mass at rest at the origin, an angular momentum along the spin
/// axis, and every disc particle on the circular orbit the stated enclosed mass
/// supports.
///
/// ## Softening is a parameter here
///
/// The circular speed follows from the acceleration a particle will actually
/// feel, and that depends on the softening the solver runs with (ADR-0008). A
/// disc built for point masses and then integrated with a softening length
/// comparable to its scale height would start out rotating too fast for the
/// forces acting on it and would expand. So the softening is one of the model's
/// parameters, and the assembly in `sim/assembly.cpp` passes the solver's value
/// through rather than letting the two disagree.

#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"

namespace orrery::initial_conditions {

/// A disc galaxy to sample.
struct DiscGalaxyParameters {
    /// The number of particles in the whole galaxy, disc and bulge together.
    ///
    /// One count rather than one per component, because the particles all carry
    /// the same mass and the split therefore follows from the two component
    /// masses rather than being independently choosable. Equal masses matter
    /// here for the same reason they matter in the Plummer sampler, and for one
    /// more: a renderer that draws every particle as the same point of light is
    /// showing mass density only if the masses are equal.
    core::Index count = 0;

    /// The mass of the exponential disc.
    core::Real disc_mass = 1;

    /// The mass of the Plummer bulge, which may be zero for a pure disc.
    ///
    /// A fifth of the disc mass by default, which is at the low end of the range
    /// observed for spiral galaxies and is chosen for the dynamics rather than
    /// for realism: a bulge concentrated enough to dominate the centre makes the
    /// inner rotation curve steep, and a steep inner rotation curve winds the
    /// spiral structure into a blur within a couple of rotations.
    core::Real bulge_mass = static_cast<core::Real>(0.2);

    /// The exponential scale length of the disc, `R_d`.
    core::Real scale_length = 1;

    /// The exponential scale height of the disc.
    ///
    /// A tenth of the scale length, which is about the ratio observed in real
    /// spirals. It is not zero because a razor-thin disc drawn as points looks
    /// like a razor-thin disc: seen edge on it disappears, and seen face on it
    /// has no depth for the camera to move through.
    core::Real scale_height = static_cast<core::Real>(0.1);

    /// The Plummer scale radius of the bulge.
    core::Real bulge_radius = static_cast<core::Real>(0.2);

    /// The fraction of the exponential disc's mass the sample is drawn from.
    ///
    /// An exponential disc has infinite extent and finite mass, so, as with the
    /// Plummer sphere, the sample has to be truncated somewhere or the outermost
    /// particle wanders off as the count rises. Ninety-nine per cent of the mass
    /// lies inside 6.64 scale lengths, which is close to where real discs are
    /// observed to end.
    core::Real mass_fraction_cutoff = static_cast<core::Real>(0.99);

    /// The Plummer softening length the run will use.
    ///
    /// Zero means the circular speeds are computed for point masses. See the
    /// note in the file comment: this should be the solver's softening, and the
    /// assembly makes it so.
    core::Real softening = 0;

    /// The angle between the disc's spin axis and the z axis, in radians.
    ///
    /// Zero puts the disc in the x-y plane rotating anticlockwise seen from
    /// above. Pi puts it in the same plane rotating the other way, which is how
    /// a retrograde encounter is asked for: there is no separate flag, because a
    /// retrograde disc is an inclined disc turned all the way over.
    core::Real inclination = 0;

    /// The angle the tilted disc is then rotated by about the z axis, in
    /// radians.
    ///
    /// Meaningless on its own, since a disc with no inclination is
    /// axisymmetric. It exists so that two inclined galaxies in an encounter can
    /// be tilted about different lines rather than both about the x axis.
    core::Real position_angle = 0;
};

/// Sample the model, in its own centre-of-mass frame at the origin.
///
/// The disc particles come first and the bulge particles follow. That ordering
/// is part of the interface rather than an accident of the implementation: the
/// tests check the disc's circular speeds and need to know which particles are
/// disc, and a renderer that tints the two components differently needs the same
/// thing. `disc_galaxy_disc_count` reports where the boundary is.
///
/// Throws `std::invalid_argument` if the disc mass is not positive, if the bulge
/// mass is negative, if any of the three lengths is not positive, if the
/// softening is negative, or if the cutoff is outside (0, 1). A zero bulge mass
/// is accepted and produces a pure disc.
[[nodiscard]] core::ParticleData make_disc_galaxy(const DiscGalaxyParameters& parameters,
                                                  core::RandomSource& random);

/// How many of the sampled particles belong to the disc.
///
/// The split is by mass, rounded to the nearest particle, so that every particle
/// in the galaxy carries the same mass. The realised component masses are
/// therefore this count and its complement times the particle mass, which is
/// what the circular speeds are computed from, rather than the requested masses.
/// The difference is at most half a particle mass and matters only in that the
/// model and the tests must agree about which it is.
[[nodiscard]] core::Index disc_galaxy_disc_count(const DiscGalaxyParameters& parameters);

/// The mass every particle in the galaxy carries.
///
/// Zero for a galaxy of no particles, which is the one case where there is no
/// answer and no caller that needs one.
[[nodiscard]] core::Real disc_galaxy_particle_mass(const DiscGalaxyParameters& parameters);

/// The total mass the sample actually has, which is the particle mass times the
/// count.
///
/// This rather than `disc_mass + bulge_mass` is what an encounter between two
/// galaxies has to be placed with, since it is the mass that will actually
/// attract.
[[nodiscard]] core::Real disc_galaxy_total_mass(const DiscGalaxyParameters& parameters);

/// The mass inside cylindrical radius `radius`, as the model computes it.
///
/// The bulge contributes its Plummer enclosed mass and the disc contributes the
/// fraction of its own sampled mass inside that radius. Exposed because it is
/// half of the circular speed below, and a test that recomputed it would be
/// checking the code against itself.
[[nodiscard]] core::Real disc_galaxy_enclosed_mass(const DiscGalaxyParameters& parameters,
                                                   core::Real radius);

/// The speed of a circular orbit at cylindrical radius `radius`, in the plane of
/// the disc.
///
/// `v^2 = G M(R) R^2 / (R^2 + eps^2)^(3/2)`, which is the softened acceleration
/// towards the centre times the radius. With no softening it reduces to the
/// familiar `G M(R) / R`.
[[nodiscard]] core::Real disc_galaxy_circular_speed(const DiscGalaxyParameters& parameters,
                                                    core::Real radius);

/// The unit vector the galaxy spins about.
///
/// The z axis carried through the inclination and the position angle. The
/// angular momentum of a sampled galaxy points along this, which is the sharpest
/// statement of the orientation that a test can check.
[[nodiscard]] core::Vec3 disc_galaxy_spin_axis(const DiscGalaxyParameters& parameters);

} // namespace orrery::initial_conditions
