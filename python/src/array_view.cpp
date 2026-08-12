#include "array_view.hpp"

#include <span>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include "orrery/core/types.hpp"

namespace py = pybind11;

namespace orrery::python {

namespace {

/// The array both overloads build, before the writeable flag is decided.
///
/// The strides are given explicitly rather than left to be inferred. A
/// component array is contiguous, so the stride is the size of one scalar, and
/// stating it keeps the shape of this call the same whichever precision the
/// library was built for.
[[nodiscard]] py::array_t<core::Real> viewing_array(const core::Real* data, core::Index count,
                                                    const py::object& owner) {
    const auto length = static_cast<py::ssize_t>(count);
    const auto stride = static_cast<py::ssize_t>(sizeof(core::Real));

    // The four-argument constructor with a non-null pointer and a base object
    // is pybind11's no-copy path: the array borrows the storage and holds a
    // reference to the base, which is released when the last view of it is.
    return {{length}, {stride}, data, owner};
}

} // namespace

py::array_t<core::Real> component_view(std::span<core::Real> component, const py::object& owner) {
    return viewing_array(component.data(), component.size(), owner);
}

py::array_t<core::Real> component_view(std::span<const core::Real> component,
                                       const py::object& owner) {
    py::array_t<core::Real> array = viewing_array(component.data(), component.size(), owner);

    // pybind11 marks an array with a non-array base as writeable whatever the
    // constness of the pointer it was given, so the flag is cleared here rather
    // than assumed. NumPy's own setflags is used for it, because the alternative
    // is reaching into the array structure through pybind11's detail namespace
    // for something the documented interface already does.
    array.attr("setflags")(py::arg("write") = false);
    return array;
}

} // namespace orrery::python
