#pragma once

/// \file
/// The Plummer sphere: a self-gravitating system that starts in equilibrium.
///
/// A star cluster sampled at random from some convenient shape is not in
/// equilibrium and spends the first part of any simulation violently rearranging
/// itself, which is a poor thing to measure conservation against. The Plummer
/// model is the standard alternative. Its density profile and its distribution
/// of velocities are an exact self-consistent solution of the collisionless
/// Boltzmann equation with Poisson's equation, so a sample drawn from it sits at
/// a virial ratio of one and stays there.
///
/// It is also the model whose potential is the softened point mass of
/// `core/softening.hpp`, which is not a coincidence: both come from the same
/// closed-form pair of density and potential, the only one simple enough to
/// invert by hand in both directions.
///
/// The sampling follows Aarseth, Henon and Wielen (1974). Radii come from
/// inverting the cumulative mass profile, which needs no rejection; speeds come
/// from rejection sampling of the distribution function, which has no closed
/// inverse. Directions for both are drawn independently and uniformly, which is
/// what makes the result isotropic.

#include <numbers>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/types.hpp"

namespace orrery::initial_conditions {

/// The scale radius that puts a unit-mass Plummer sphere into standard N-body
/// units, where the total energy is exactly `-1/4`.
///
/// `3 pi / 16`. The convention is Heggie and Mathieu's, and it is worth
/// following rather than inventing: nearly every published figure for cluster
/// simulations is quoted in these units, so a result computed here can be put
/// beside one from the literature without a scaling factor standing between
/// them.
inline constexpr core::Real kStandardPlummerRadius = 3 * std::numbers::pi_v<core::Real> / 16;

/// A Plummer sphere to sample.
struct PlummerParameters {
    core::Index count = 0;

    /// Shared equally among the particles.
    ///
    /// Equal masses because the cluster this represents has them, and because
    /// unequal masses would introduce mass segregation into a configuration
    /// whose purpose is to be simple enough to reason about. A mass spectrum is
    /// a modelling choice for a later study, not a property of the reference
    /// configuration.
    core::Real total_mass = 1;

    core::Real scale_radius = kStandardPlummerRadius;

    /// The fraction of the model's mass the sample is drawn from.
    ///
    /// The Plummer sphere is infinite: its mass converges but its radius does
    /// not, and the inverted mass profile sends the outermost sampled particle
    /// to a radius that grows without bound as the count rises. One particle a
    /// thousand scale radii out contributes almost nothing to the potential
    /// energy and a great deal to the bounding box, the tree depth, and the
    /// timestep the outer orbit needs.
    ///
    /// Truncating at 99.9 per cent of the mass bounds the sample at 38.7 scale
    /// radii while moving a thousandth of the mass inwards, which shifts the
    /// energies by far less than the sampling noise at any count this project
    /// runs. It is a parameter rather than a constant so that a study which
    /// needs the tail can ask for it.
    core::Real mass_fraction_cutoff = static_cast<core::Real>(0.999);
};

/// Sample `count` particles from the model, in the centre-of-mass frame.
///
/// Throws `std::invalid_argument` if the total mass or the scale radius is not
/// positive, or if the cutoff is outside (0, 1).
[[nodiscard]] core::ParticleData make_plummer_sphere(const PlummerParameters& parameters,
                                                     core::RandomSource& random);

/// The potential energy of the model, `-3 pi G M^2 / 32 a`.
///
/// The continuum value, which a sample approaches as the count rises. A finite
/// sample departs from it in two ways worth knowing about when a tolerance is
/// chosen: the statistical scatter of the draw, which falls as the square root
/// of the count, and a deficit of one part in the count, because a sample of N
/// particles has N(N-1)/2 interacting pairs where the continuum has, in effect,
/// N^2/2.
[[nodiscard]] core::Real plummer_potential_energy(const PlummerParameters& parameters);

/// The kinetic energy of the model, `3 pi G M^2 / 64 a`.
///
/// Exactly half the magnitude of the potential energy, which is the virial
/// theorem for this system and the statement the sampler is validated against.
[[nodiscard]] core::Real plummer_kinetic_energy(const PlummerParameters& parameters);

/// The total energy of the model, `-3 pi G M^2 / 64 a`.
///
/// Equal to `-1/4` for a unit-mass sphere at the standard scale radius above.
[[nodiscard]] core::Real plummer_total_energy(const PlummerParameters& parameters);

} // namespace orrery::initial_conditions
