#pragma once

/// \file
/// A uniform sphere of cold particles: the configuration for scaling work.
///
/// Performance measurements need a configuration whose only interesting
/// property is how many particles are in it. A Plummer sphere is not that: its
/// density spans several orders of magnitude between the centre and the edge,
/// so the tree depth in Phase 8 varies across the volume and a timing taken
/// from it mixes the cost of the algorithm with the shape of the model. A
/// uniform sphere has one density, a bounded radius, and a tree that is the same
/// depth everywhere, which makes it the honest thing to quote a particles-per-
/// second figure from.
///
/// It is also a physics test in its own right. The particles start at rest, so
/// the configuration is the classic cold collapse: it has half the kinetic
/// energy the virial theorem asks for at every radius, falls inward, and passes
/// through a state dense enough to punish an integrator with a fixed timestep.
/// That makes it the natural stress case for the energy conservation results,
/// as against the Plummer sphere, which is the natural equilibrium case.
///
/// Its potential energy has a closed form, `-3 G M^2 / 5 R`, which is the
/// simplest analytic check available on the potential energy diagnostic and
/// needs no sampling theory to interpret.

#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/types.hpp"

namespace orrery::initial_conditions {

/// A sphere of uniform density to sample.
struct UniformSphereParameters {
    core::Index count = 0;

    /// Shared equally among the particles, so that the sampled density is
    /// uniform in mass as well as in number.
    core::Real total_mass = 1;

    core::Real radius = 1;
};

/// Sample `count` particles uniformly through the sphere, at rest.
///
/// Positions are recentred so the centre of mass is at the origin. The
/// velocities are exactly zero and are left alone, since a configuration at
/// rest is already in its own centre-of-mass frame.
///
/// Throws `std::invalid_argument` if the total mass or the radius is not
/// positive.
[[nodiscard]] core::ParticleData make_uniform_sphere(const UniformSphereParameters& parameters,
                                                     core::RandomSource& random);

/// The potential energy of the model, `-3 G M^2 / 5 R`.
///
/// The continuum value. A finite sample scatters about it, and, as with the
/// Plummer sphere, falls short of it by about one part in the count because a
/// sample has no self-pairs.
[[nodiscard]] core::Real uniform_sphere_potential_energy(const UniformSphereParameters& parameters);

} // namespace orrery::initial_conditions
