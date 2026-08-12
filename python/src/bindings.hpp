#pragma once

/// \file
/// The four halves of the module definition.
///
/// One function per group of types rather than one long module body, for the
/// same reason the test suite has one executable per layer: a binding for a
/// configuration structure and a binding for a NumPy view have nothing to say
/// to each other, and a single translation unit holding both takes noticeably
/// longer to compile than either. pybind11's template machinery is the reason
/// that matters here; it is the slowest thing this project compiles.

#include <pybind11/pybind11.h>

namespace orrery::python {

/// Scalars, vectors, the particle store and the conserved quantities.
void bind_core(pybind11::module_& module);

/// The samplers and the analytic configurations, with their closed-form
/// answers beside them.
void bind_initial_conditions(pybind11::module_& module);

/// The configuration record, the enumerations it selects with, and the
/// simulation it assembles into.
void bind_sim(pybind11::module_& module);

} // namespace orrery::python
