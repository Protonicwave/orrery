#pragma once

/// \file
/// NumPy arrays that view the particle store rather than copy it.
///
/// This is the file the phase exists for. A binding that handed NumPy a copy of
/// each component array would be correct and would also be useless at the sizes
/// this project runs at: a million particles is 80 MB of state in double
/// precision, and a notebook that plotted every hundredth step of a run would
/// spend more time copying than the simulation spent integrating.
///
/// So the arrays returned here are views. The array object holds no storage of
/// its own; it points at the component array inside the `ParticleData` and
/// names the Python object that owns it as its base, which is what keeps that
/// object alive for as long as any view of it exists. Writing through the view
/// writes into the simulation's state, and that is the intended behaviour
/// rather than a leak: setting up a configuration from Python means assigning
/// into these arrays.
///
/// ## Why they are one-dimensional and one per component
///
/// The storage is a separate contiguous array per component (ADR-0004), so an
/// N by 3 array of positions does not exist anywhere in memory and cannot be
/// viewed into being. Presenting one would mean copying, and a copy that looks
/// like a view is worse than an honest copy: a caller who wrote into it would
/// see the write disappear. ADR-0040 records the decision to expose the layout
/// as it is.
///
/// ## Lifetime, and the one way to break it
///
/// A view survives its owner because of the base reference. It does not survive
/// a reallocation: `resize`, `reserve` and `restore` may move the component
/// arrays, and a view taken before one of those points at freed memory
/// afterwards, exactly as a `std::span` would. There is no way to detect that
/// from inside NumPy, so it is documented on each of the operations that can
/// cause it rather than guarded against.

#include <span>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include "orrery/core/types.hpp"

namespace orrery::python {

/// A writable NumPy view of one component array.
///
/// `owner` is the Python object that keeps the storage alive, and becomes the
/// array's base. It is the object holding the arrays, not the array itself: a
/// view of a simulation's state names the simulation, since that is what owns
/// the memory.
[[nodiscard]] pybind11::array_t<core::Real> component_view(std::span<core::Real> component,
                                                           const pybind11::object& owner);

/// A read-only NumPy view of one component array.
///
/// The overload is selected by the constness of the span, so a caller cannot
/// hand out a writable view of something it only has read access to by
/// forgetting a flag. The array's `writeable` flag is cleared, which is what
/// makes an attempt to write raise rather than corrupt a running simulation.
[[nodiscard]] pybind11::array_t<core::Real> component_view(std::span<const core::Real> component,
                                                           const pybind11::object& owner);

} // namespace orrery::python
