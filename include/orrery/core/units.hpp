#pragma once

/// \file
/// The unit system every quantity in the project is expressed in.
///
/// Orrery works in N-body units: the gravitational constant is one, and mass,
/// length and time are whatever the caller's configuration makes them. This is
/// the convention the stellar dynamics literature states its results in, so a
/// virial ratio or a Plummer energy computed here can be compared against a
/// published figure without a conversion standing between them.
///
/// The alternative, carrying SI or astronomical values, would put a factor of
/// 6.674e-11 or 4pi^2 into the innermost multiplication of every force
/// evaluation, and would make the dynamic range of the intermediate products
/// far wider than it needs to be. In single precision, which is the
/// configuration the largest runs use, that range is not free.
///
/// A caller with a system in physical units scales it on the way in. That
/// conversion belongs to the configuration layer that reads the run
/// description, not to the kernels, which should see numbers of order one.
/// ADR-0007 records the alternatives.

#include "orrery/core/types.hpp"

namespace orrery::core {

/// The gravitational constant, one by the definition of the unit system.
///
/// Named rather than left implicit so that the places where it belongs are
/// visible in the source. An expression for a potential energy that shows no
/// `G` at all reads as though the physics were forgotten, and a reader cannot
/// tell the difference between a deliberate choice of units and an omission.
///
/// It is a constant rather than a parameter for the same reason the softening
/// is shared rather than passed independently to each caller: the unit system
/// is a property of the whole simulation, and letting a force kernel and an
/// energy diagnostic each take their own value is how the two come to disagree.
inline constexpr Real kGravitationalConstant = 1;

} // namespace orrery::core
