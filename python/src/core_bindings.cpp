#include <limits>
#include <sstream>
#include <string>

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>

#include "array_view.hpp"
#include "bindings.hpp"
#include "orrery/core/diagnostics.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "particle_view.hpp"

namespace py = pybind11;
using namespace pybind11::literals;

using orrery::core::Diagnostics;
using orrery::core::Index;
using orrery::core::ParticleData;
using orrery::core::Real;
using orrery::core::Softening;
using orrery::core::Vec3;

namespace orrery::python {

namespace {

/// The number of digits that distinguishes any two scalars of this build.
///
/// A repr is what a person copies into a bug report, so it has to round-trip:
/// a position printed to six digits and pasted back is a different position,
/// and the difference is far larger than anything this project claims to
/// conserve.
[[nodiscard]] std::string full_precision(Real value) {
    std::ostringstream text;
    text.precision(std::numeric_limits<Real>::max_digits10);
    text << value;
    return text.str();
}

[[nodiscard]] std::string vec3_repr(Vec3 value) {
    return "Vec3(" + full_precision(value.x) + ", " + full_precision(value.y) + ", " +
           full_precision(value.z) + ")";
}

/// One component of a 3-vector, indexed as Python indexes a sequence.
[[nodiscard]] Real vec3_component(Vec3 value, py::ssize_t index) {
    const py::ssize_t position = index < 0 ? index + 3 : index;
    if (position == 0) {
        return value.x;
    }
    if (position == 1) {
        return value.y;
    }
    if (position == 2) {
        return value.z;
    }
    throw py::index_error("a 3-vector has three components");
}

void bind_vec3(py::module_& module) {
    py::class_<Vec3>(module, "Vec3", R"(A three-component vector.

Bound as a class rather than converted to a NumPy array of three, because it
appears in this interface as a single quantity: a centre of mass, a momentum, a
position handed to ``ParticleData.add``. It is a sequence of three, so
``tuple(v)``, ``list(v)`` and ``numpy.asarray(v)`` all do what they look like,
and unpacking with ``x, y, z = v`` works.)")
        .def(py::init([](Real x, Real y, Real z) { return Vec3{x, y, z}; }), "x"_a = Real{0},
             "y"_a = Real{0}, "z"_a = Real{0})
        // The second constructor is what makes a tuple usable wherever a vector
        // is expected. It is tried after the one above, so three separate
        // arguments still reach the three-argument form.
        .def(py::init([](const py::sequence& components) {
            if (py::len(components) != 3) {
                throw py::value_error("a 3-vector is built from three components");
            }
            return Vec3{components[0].cast<Real>(), components[1].cast<Real>(),
                        components[2].cast<Real>()};
        }))
        .def_readwrite("x", &Vec3::x)
        .def_readwrite("y", &Vec3::y)
        .def_readwrite("z", &Vec3::z)
        .def("__len__", [](const Vec3&) { return 3; })
        .def("__getitem__", &vec3_component)
        .def("__repr__", &vec3_repr)
        // `py::self` is pybind11's placeholder for the bound type rather than a
        // value, so both sides of each operator below are deliberately the same
        // expression and the redundancy check has nothing to act on.
        // NOLINTBEGIN(misc-redundant-expression)
        .def(py::self == py::self)
        .def(py::self + py::self)
        .def(py::self - py::self)
        .def(-py::self)
        .def(py::self * Real{})
        .def(Real{} * py::self)
        .def(py::self / Real{})
        .def(py::self += py::self)
        .def(py::self -= py::self)
        .def(py::self *= Real{})
        .def(py::self /= Real{});
    // NOLINTEND(misc-redundant-expression)

    // A tuple where a vector is expected is what a person writes, and refusing
    // it would make every call site say Vec3 twice.
    py::implicitly_convertible<py::tuple, Vec3>();
    py::implicitly_convertible<py::list, Vec3>();

    module.def("dot", &orrery::core::dot, "a"_a, "b"_a, "The scalar product of two 3-vectors.");
    module.def("cross", &orrery::core::cross, "a"_a, "b"_a, "The vector product of two 3-vectors.");
    module.def("norm", &orrery::core::norm, "v"_a, "The length of a 3-vector.");
}

void bind_softening(py::module_& module) {
    py::class_<Softening>(module, "Softening", R"(A Plummer softening length.

A separate type rather than a bare number for the reason ADR-0008 gives: the
potential energy of a configuration has to be computed with the same softening
as the force on it, and a quantity that carries its own name is harder to pass
to one and forget to pass to the other. A float is accepted anywhere one of
these is expected, and means a softening of that length.)")
        .def(py::init<>())
        .def(py::init<Real>(), "length"_a)
        .def_property_readonly("length", &Softening::length)
        .def_property_readonly("squared", &Softening::squared)
        .def("__repr__",
             [](Softening value) { return "Softening(" + full_precision(value.length()) + ")"; })
        .def(
            "__eq__",
            [](Softening value, Softening other) { return value.squared() == other.squared(); },
            py::is_operator());

    py::implicitly_convertible<py::float_, Softening>();
    py::implicitly_convertible<py::int_, Softening>();
}

void bind_diagnostics(py::module_& module) {
    py::class_<Diagnostics>(module, "Diagnostics", R"(The conserved quantities at one instant.

The two derived quantities are properties rather than stored fields, so they
cannot be stale. ``virial_ratio`` is ``2T / |U|``, which is one for a
self-gravitating system in equilibrium; the convention matters, since some
authors write the same statement as ``T / |U|`` and get a half.)")
        .def(py::init<>())
        .def_readwrite("kinetic_energy", &Diagnostics::kinetic_energy)
        .def_readwrite("potential_energy", &Diagnostics::potential_energy)
        .def_readwrite("linear_momentum", &Diagnostics::linear_momentum)
        .def_readwrite("angular_momentum", &Diagnostics::angular_momentum)
        .def_property_readonly("total_energy", &Diagnostics::total_energy)
        .def_property_readonly("virial_ratio", &Diagnostics::virial_ratio)
        .def("__repr__", [](const Diagnostics& value) {
            return "Diagnostics(kinetic_energy=" + full_precision(value.kinetic_energy) +
                   ", potential_energy=" + full_precision(value.potential_energy) +
                   ", total_energy=" + full_precision(value.total_energy()) +
                   ", virial_ratio=" + full_precision(value.virial_ratio()) + ")";
        });

    module.def(
        "total_mass", [](const ParticleData& data) { return core::total_mass(data.masses()); },
        "particles"_a, "The sum of the masses.");

    module.def(
        "centre_of_mass",
        [](const ParticleData& data) {
            return core::centre_of_mass(data.positions(), data.masses());
        },
        "particles"_a, "The mass-weighted mean position.");

    module.def(
        "centre_of_mass_velocity",
        [](const ParticleData& data) {
            return core::centre_of_mass_velocity(data.velocities(), data.masses());
        },
        "particles"_a, "The velocity of the centre of mass.");

    module.def(
        "kinetic_energy",
        [](const ParticleData& data) {
            return core::kinetic_energy(data.velocities(), data.masses());
        },
        "particles"_a, "The total kinetic energy.");

    module.def(
        "linear_momentum",
        [](const ParticleData& data) {
            return core::linear_momentum(data.velocities(), data.masses());
        },
        "particles"_a, "The total linear momentum.");

    module.def(
        "angular_momentum",
        [](const ParticleData& data) {
            return core::angular_momentum(data.positions(), data.velocities(), data.masses());
        },
        "particles"_a, "The total angular momentum, about the origin.");

    // The two that cost an N^2 pass release the interpreter lock. Everything
    // above is one pass over the particles and would spend more time on the
    // handover than on the work.
    module.def(
        "potential_energy",
        [](const ParticleData& data, Softening softening) {
            return core::potential_energy(data.positions(), data.masses(), softening);
        },
        "particles"_a, "softening"_a = Softening{},
        "The total potential energy, softened as given. Costs an N^2 pass.",
        py::call_guard<py::gil_scoped_release>());

    module.def(
        "measure_diagnostics",
        [](const ParticleData& data, Softening softening) {
            return core::measure_diagnostics(data, softening);
        },
        "particles"_a, "softening"_a = Softening{},
        R"(Every conserved quantity of a configuration, measured together.

The softening must be the one the forces were computed with, or the result
measures the mismatch rather than the physics. ``Simulation.measure`` asks its
own solver and cannot get that wrong, and is what a run should use.)",
        py::call_guard<py::gil_scoped_release>());
}

void bind_particle_data(py::module_& module) {
    py::class_<ParticleData>(module, "ParticleData", R"(Storage for a set of point masses.

The ten component arrays are exposed as ten NumPy views, one per array, because
that is what the storage is: positions are held as three contiguous arrays of x,
y and z rather than as one array of triples (ADR-0004), so an N by 3 array of
positions exists nowhere in memory and could only be produced by copying. The
views are writable and share memory with the simulation, so assigning into
``position_x`` moves particles.

A view is invalidated by anything that can reallocate: ``resize``, ``reserve``,
``add``, and ``Simulation.restore``. Take the view again afterwards.)")
        .def(py::init<>())
        .def(py::init<Index>(), "count"_a, "Create `count` particles with every component zero.")
        .def("__len__", &ParticleData::size)
        .def_property_readonly("size", &ParticleData::size)
        .def_property_readonly("capacity", &ParticleData::capacity)
        .def("reserve", &ParticleData::reserve, "count"_a,
             "Make room for `count` particles. Invalidates every view.")
        .def("resize", &ParticleData::resize, "count"_a,
             "Change the number of particles, zero-filling any new ones. Invalidates every view.")
        .def("clear", &ParticleData::clear, "Remove every particle, keeping the storage.")
        .def("add", &ParticleData::add, "position"_a, "velocity"_a, "mass"_a,
             "Append one particle with zero acceleration. Invalidates every view.")
        .def(
            "copy", [](const ParticleData& data) { return ParticleData(data); },
            "An independent copy, sharing no memory with this one.")
        .def_property_readonly("position_x",
                               [](const py::object& self) {
                                   return component_view(self.cast<ParticleData&>().positions().x,
                                                         self);
                               })
        .def_property_readonly("position_y",
                               [](const py::object& self) {
                                   return component_view(self.cast<ParticleData&>().positions().y,
                                                         self);
                               })
        .def_property_readonly("position_z",
                               [](const py::object& self) {
                                   return component_view(self.cast<ParticleData&>().positions().z,
                                                         self);
                               })
        .def_property_readonly("velocity_x",
                               [](const py::object& self) {
                                   return component_view(self.cast<ParticleData&>().velocities().x,
                                                         self);
                               })
        .def_property_readonly("velocity_y",
                               [](const py::object& self) {
                                   return component_view(self.cast<ParticleData&>().velocities().y,
                                                         self);
                               })
        .def_property_readonly("velocity_z",
                               [](const py::object& self) {
                                   return component_view(self.cast<ParticleData&>().velocities().z,
                                                         self);
                               })
        .def_property_readonly("acceleration_x",
                               [](const py::object& self) {
                                   return component_view(
                                       self.cast<ParticleData&>().accelerations().x, self);
                               })
        .def_property_readonly("acceleration_y",
                               [](const py::object& self) {
                                   return component_view(
                                       self.cast<ParticleData&>().accelerations().y, self);
                               })
        .def_property_readonly("acceleration_z",
                               [](const py::object& self) {
                                   return component_view(
                                       self.cast<ParticleData&>().accelerations().z, self);
                               })
        .def_property_readonly("mass",
                               [](const py::object& self) {
                                   return component_view(self.cast<ParticleData&>().masses(), self);
                               })
        .def("__repr__", [](const ParticleData& data) {
            return "<ParticleData with " + std::to_string(data.size()) + " particles>";
        });
}

void bind_particle_view(py::module_& module) {
    py::class_<ParticleView>(module, "ParticleView", R"(A read-only view of a simulation's state.

The same ten component arrays as ``ParticleData``, with the writeable flag
cleared. It is read-only because the object it refers to is: the integrators
require the acceleration array to hold the acceleration at the current
positions on entry to every step, and writing into a running simulation would
break that without any way for it to notice. ``Simulation.restore`` is how a
state is put into a simulation, and it re-establishes the invariant.

Holds no storage. It keeps the simulation alive for as long as it or any array
taken from it exists, and is invalidated by ``Simulation.restore``.)")
        .def("__len__", [](const ParticleView& view) { return view.data().size(); })
        .def_property_readonly("size", [](const ParticleView& view) { return view.data().size(); })
        .def(
            "copy", [](const ParticleView& view) { return ParticleData(view.data()); },
            "An independent, writable copy of the state.")
        .def_property_readonly("position_x",
                               [](const ParticleView& view) {
                                   return component_view(view.data().positions().x, view.owner());
                               })
        .def_property_readonly("position_y",
                               [](const ParticleView& view) {
                                   return component_view(view.data().positions().y, view.owner());
                               })
        .def_property_readonly("position_z",
                               [](const ParticleView& view) {
                                   return component_view(view.data().positions().z, view.owner());
                               })
        .def_property_readonly("velocity_x",
                               [](const ParticleView& view) {
                                   return component_view(view.data().velocities().x, view.owner());
                               })
        .def_property_readonly("velocity_y",
                               [](const ParticleView& view) {
                                   return component_view(view.data().velocities().y, view.owner());
                               })
        .def_property_readonly("velocity_z",
                               [](const ParticleView& view) {
                                   return component_view(view.data().velocities().z, view.owner());
                               })
        .def_property_readonly("acceleration_x",
                               [](const ParticleView& view) {
                                   return component_view(view.data().accelerations().x,
                                                         view.owner());
                               })
        .def_property_readonly("acceleration_y",
                               [](const ParticleView& view) {
                                   return component_view(view.data().accelerations().y,
                                                         view.owner());
                               })
        .def_property_readonly("acceleration_z",
                               [](const ParticleView& view) {
                                   return component_view(view.data().accelerations().z,
                                                         view.owner());
                               })
        .def_property_readonly("mass",
                               [](const ParticleView& view) {
                                   return component_view(view.data().masses(), view.owner());
                               })
        .def("__repr__", [](const ParticleView& view) {
            return "<ParticleView of " + std::to_string(view.data().size()) + " particles>";
        });
}

} // namespace

void bind_core(py::module_& module) {
    bind_vec3(module);
    bind_softening(module);
    bind_particle_data(module);
    bind_particle_view(module);
    bind_diagnostics(module);
}

} // namespace orrery::python
