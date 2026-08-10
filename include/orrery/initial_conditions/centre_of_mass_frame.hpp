#pragma once

/// \file
/// Moving a sampled configuration into its own centre-of-mass frame.
///
/// A configuration drawn from a spherically symmetric model has a centre of
/// mass near the origin and a net momentum near zero, but only near: both are
/// sums of N random draws and so are of order the square root of N away from
/// the values the model has. Left alone, that residue drifts the whole system
/// across the volume during a long integration, and it puts a term into the
/// angular momentum that depends on where the system happens to be rather than
/// on how it is rotating.
///
/// It also spoils the tests. A property test asserting that total momentum is
/// conserved to round-off is far sharper when the conserved value is zero, and
/// an angular momentum measured about the origin only equals the one measured
/// about the centre of mass when the two coincide.
///
/// This is a step a sampler applies to itself, not something a caller must
/// remember; it is public because the tests check it directly and because a
/// configuration assembled by hand from several pieces needs it too.

#include "orrery/core/particle_data.hpp"

namespace orrery::initial_conditions {

/// Shift positions so the centre of mass is at the origin, and velocities so
/// the total momentum is zero.
///
/// Both shifts are subtractions of one vector from every particle, so the
/// internal structure of the configuration is untouched: separations, and
/// therefore the potential energy, are exactly what they were, and relative
/// velocities are too.
///
/// The result is not exactly centred, because the mean of the shifted values is
/// itself a rounded sum. It is centred to round-off, which is the accuracy the
/// conservation tests are written at.
///
/// Configurations that are exact by construction, the Kepler two-body among
/// them, do not use this. Applying it would replace their exact zero with a
/// rounded one.
void move_to_centre_of_mass_frame(core::ParticleData& data);

} // namespace orrery::initial_conditions
