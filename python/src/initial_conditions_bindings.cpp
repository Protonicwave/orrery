#include <cstdint>

#include <pybind11/pybind11.h>

#include "bindings.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/types.hpp"
#include "orrery/initial_conditions/centre_of_mass_frame.hpp"
#include "orrery/initial_conditions/disc_galaxy.hpp"
#include "orrery/initial_conditions/galaxy_collision.hpp"
#include "orrery/initial_conditions/kepler.hpp"
#include "orrery/initial_conditions/plummer.hpp"
#include "orrery/initial_conditions/uniform_sphere.hpp"

namespace py = pybind11;
using namespace pybind11::literals;

using orrery::core::Index;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;

namespace ic = orrery::initial_conditions;

namespace orrery::python {

namespace {

/// Bind a sampler twice: once taking a seed and once taking a stream.
///
/// The two differ in more than convenience. Two models drawn from two sources
/// seeded the same way are the same model, which is almost never what a caller
/// asking for two of them wants, so anything drawing more than one configuration
/// passes one source to both and gets independent draws. The seed form is the
/// short way to say the common case, and both record what they drew from.
template<typename Parameters, typename Sampler>
void bind_sampler(py::module_& module, const char* name, Sampler sampler, const char* description) {
    module.def(
        name,
        [sampler](const Parameters& parameters, std::uint64_t seed) {
            RandomSource random(seed);
            return sampler(parameters, random);
        },
        "parameters"_a, "seed"_a = std::uint64_t{0}, description);

    module.def(
        name,
        [sampler](const Parameters& parameters, RandomSource& random) {
            return sampler(parameters, random);
        },
        "parameters"_a, "random"_a, description);
}

void bind_random(py::module_& module) {
    py::class_<RandomSource>(module, "RandomSource", R"(A deterministic stream of random numbers.

Seeded explicitly and never from the clock, because a sampled configuration that
differs between runs makes every result that follows from it unreproducible. The
seed is kept so that a configuration can say which stream produced it.)")
        .def(py::init<std::uint64_t>(), "seed"_a)
        .def_property_readonly("seed", &RandomSource::seed)
        // Spelled with static_cast rather than pybind11's overload_cast because
        // both overloads are noexcept, which is part of the function type since
        // C++17 and which overload_cast does not carry.
        .def("uniform", static_cast<Real (RandomSource::*)() noexcept>(&RandomSource::uniform),
             "A draw from [0, 1).")
        .def("uniform",
             static_cast<Real (RandomSource::*)(Real, Real) noexcept>(&RandomSource::uniform),
             "low"_a, "high"_a, "A draw from [low, high).")
        .def("unit_vector", &RandomSource::unit_vector,
             "A direction drawn uniformly over the sphere.");
}

void bind_plummer(py::module_& module) {
    module.attr("standard_plummer_radius") = ic::kStandardPlummerRadius;

    py::class_<ic::PlummerParameters>(module, "PlummerParameters",
                                      R"(A Plummer sphere, as parameters.

The default scale radius is the value that puts a unit-mass sphere into standard
N-body units, where the total energy is -1/4 and the virial radius is one.

The same object is passed to the sampler and to the closed-form energies below,
so a comparison between a sample and the model it was drawn from cannot be made
against different parameters by accident.)")
        .def(py::init(
                 [](Index count, Real total_mass, Real scale_radius, Real mass_fraction_cutoff) {
                     return ic::PlummerParameters{count, total_mass, scale_radius,
                                                  mass_fraction_cutoff};
                 }),
             "count"_a = Index{0}, "total_mass"_a = Real{1},
             "scale_radius"_a = ic::kStandardPlummerRadius,
             "mass_fraction_cutoff"_a = static_cast<Real>(0.999))
        .def_readwrite("count", &ic::PlummerParameters::count)
        .def_readwrite("total_mass", &ic::PlummerParameters::total_mass)
        .def_readwrite("scale_radius", &ic::PlummerParameters::scale_radius)
        .def_readwrite("mass_fraction_cutoff", &ic::PlummerParameters::mass_fraction_cutoff);

    bind_sampler<ic::PlummerParameters>(module, "plummer_sphere", &ic::make_plummer_sphere,
                                        "Sample a Plummer sphere.");

    module.def("plummer_potential_energy", &ic::plummer_potential_energy, "parameters"_a,
               "The potential energy of the model, in closed form.");
    module.def("plummer_kinetic_energy", &ic::plummer_kinetic_energy, "parameters"_a,
               "The kinetic energy of the model in equilibrium, in closed form.");
    module.def("plummer_total_energy", &ic::plummer_total_energy, "parameters"_a,
               "The total energy of the model, in closed form.");
}

void bind_uniform_sphere(py::module_& module) {
    py::class_<ic::UniformSphereParameters>(module, "UniformSphereParameters",
                                            "A sphere of uniform density, as parameters.")
        .def(py::init([](Index count, Real total_mass, Real radius) {
                 return ic::UniformSphereParameters{count, total_mass, radius};
             }),
             "count"_a = Index{0}, "total_mass"_a = Real{1}, "radius"_a = Real{1})
        .def_readwrite("count", &ic::UniformSphereParameters::count)
        .def_readwrite("total_mass", &ic::UniformSphereParameters::total_mass)
        .def_readwrite("radius", &ic::UniformSphereParameters::radius);

    bind_sampler<ic::UniformSphereParameters>(module, "uniform_sphere", &ic::make_uniform_sphere,
                                              "Sample a sphere of uniform density.");

    module.def("uniform_sphere_potential_energy", &ic::uniform_sphere_potential_energy,
               "parameters"_a, "The potential energy of the model, `-3 G M^2 / 5 R`.");
}

void bind_kepler(py::module_& module) {
    py::class_<ic::KeplerParameters>(module, "KeplerParameters",
                                     R"(An exact two-body orbit, as parameters.

Constructed rather than sampled, and released at periapsis, so the state after
one period is the state it started in. That is what makes it the instrument the
integrators are measured with.)")
        .def(py::init([](Real primary_mass, Real secondary_mass, Real semi_major_axis,
                         Real eccentricity) {
                 return ic::KeplerParameters{primary_mass, secondary_mass, semi_major_axis,
                                             eccentricity};
             }),
             "primary_mass"_a = Real{1}, "secondary_mass"_a = Real{1},
             "semi_major_axis"_a = Real{1}, "eccentricity"_a = Real{0})
        .def_readwrite("primary_mass", &ic::KeplerParameters::primary_mass)
        .def_readwrite("secondary_mass", &ic::KeplerParameters::secondary_mass)
        .def_readwrite("semi_major_axis", &ic::KeplerParameters::semi_major_axis)
        .def_readwrite("eccentricity", &ic::KeplerParameters::eccentricity);

    module.def("kepler_orbit", &ic::make_kepler_orbit, "parameters"_a,
               "The two bodies, at periapsis, in the centre of mass frame.");
    module.def("kepler_period", &ic::kepler_period, "parameters"_a,
               "The orbital period, `2 pi sqrt(a^3 / G M)`.");
    module.def("kepler_energy", &ic::kepler_energy, "parameters"_a,
               "The total energy of the orbit.");
    module.def("kepler_angular_momentum", &ic::kepler_angular_momentum, "parameters"_a,
               "The magnitude of the orbital angular momentum.");
    module.def("kepler_periapsis_distance", &ic::kepler_periapsis_distance, "parameters"_a,
               "The separation at periapsis, `a (1 - e)`.");
}

void bind_disc_galaxy(py::module_& module) {
    py::class_<ic::DiscGalaxyParameters>(module, "DiscGalaxyParameters",
                                         R"(A disc galaxy with a bulge, as parameters.

An exponential disc in radius and an isothermal sheet in height, on circular
orbits about a Plummer bulge, with the disc's own enclosed mass included in the
circular speed. It is a plausible-looking galaxy rather than an equilibrium one:
see ADR-0038 for what it does and does not claim to be.)")
        .def(py::init([](Index count, Real disc_mass, Real bulge_mass, Real scale_length,
                         Real scale_height, Real bulge_radius, Real mass_fraction_cutoff,
                         Real softening, Real inclination, Real position_angle) {
                 return ic::DiscGalaxyParameters{
                     count,        disc_mass,     bulge_mass,           scale_length,
                     scale_height, bulge_radius,  mass_fraction_cutoff, softening,
                     inclination,  position_angle};
             }),
             "count"_a = Index{0}, "disc_mass"_a = Real{1}, "bulge_mass"_a = static_cast<Real>(0.2),
             "scale_length"_a = Real{1}, "scale_height"_a = static_cast<Real>(0.1),
             "bulge_radius"_a = static_cast<Real>(0.2),
             "mass_fraction_cutoff"_a = static_cast<Real>(0.99), "softening"_a = Real{0},
             "inclination"_a = Real{0}, "position_angle"_a = Real{0})
        .def_readwrite("count", &ic::DiscGalaxyParameters::count)
        .def_readwrite("disc_mass", &ic::DiscGalaxyParameters::disc_mass)
        .def_readwrite("bulge_mass", &ic::DiscGalaxyParameters::bulge_mass)
        .def_readwrite("scale_length", &ic::DiscGalaxyParameters::scale_length)
        .def_readwrite("scale_height", &ic::DiscGalaxyParameters::scale_height)
        .def_readwrite("bulge_radius", &ic::DiscGalaxyParameters::bulge_radius)
        .def_readwrite("mass_fraction_cutoff", &ic::DiscGalaxyParameters::mass_fraction_cutoff)
        .def_readwrite("softening", &ic::DiscGalaxyParameters::softening)
        .def_readwrite("inclination", &ic::DiscGalaxyParameters::inclination)
        .def_readwrite("position_angle", &ic::DiscGalaxyParameters::position_angle);

    bind_sampler<ic::DiscGalaxyParameters>(module, "disc_galaxy", &ic::make_disc_galaxy,
                                           "Sample a disc galaxy.");

    module.def("disc_galaxy_disc_count", &ic::disc_galaxy_disc_count, "parameters"_a,
               "How many of the particles belong to the disc rather than the bulge.");
    module.def("disc_galaxy_particle_mass", &ic::disc_galaxy_particle_mass, "parameters"_a,
               "The mass each particle carries.");
    module.def("disc_galaxy_total_mass", &ic::disc_galaxy_total_mass, "parameters"_a,
               "The disc mass plus the bulge mass.");
    module.def("disc_galaxy_enclosed_mass", &ic::disc_galaxy_enclosed_mass, "parameters"_a,
               "radius"_a, "The mass inside a given radius, disc and bulge together.");
    module.def("disc_galaxy_circular_speed", &ic::disc_galaxy_circular_speed, "parameters"_a,
               "radius"_a, "The speed of a circular orbit at a given radius.");
    module.def("disc_galaxy_spin_axis", &ic::disc_galaxy_spin_axis, "parameters"_a,
               "The unit vector the disc turns about.");

    py::class_<ic::GalaxyCollisionParameters>(module, "GalaxyCollisionParameters",
                                              R"(Two disc galaxies on an encounter.

The approach speed is a multiple of the escape speed at the initial separation,
so below one the pair is bound and merges.)")
        .def(py::init([](const ic::DiscGalaxyParameters& primary,
                         const ic::DiscGalaxyParameters& secondary, Real separation,
                         Real impact_parameter, Real approach_speed) {
                 return ic::GalaxyCollisionParameters{primary, secondary, separation,
                                                      impact_parameter, approach_speed};
             }),
             "primary"_a = ic::DiscGalaxyParameters{}, "secondary"_a = ic::DiscGalaxyParameters{},
             "separation"_a = Real{20}, "impact_parameter"_a = Real{2},
             "approach_speed"_a = static_cast<Real>(0.8))
        .def_readwrite("primary", &ic::GalaxyCollisionParameters::primary)
        .def_readwrite("secondary", &ic::GalaxyCollisionParameters::secondary)
        .def_readwrite("separation", &ic::GalaxyCollisionParameters::separation)
        .def_readwrite("impact_parameter", &ic::GalaxyCollisionParameters::impact_parameter)
        .def_readwrite("approach_speed", &ic::GalaxyCollisionParameters::approach_speed);

    bind_sampler<ic::GalaxyCollisionParameters>(module, "galaxy_collision",
                                                &ic::make_galaxy_collision,
                                                "Sample two galaxies set on an encounter.");

    module.def("galaxy_collision_separation", &ic::galaxy_collision_separation, "parameters"_a,
               "The vector from the first galaxy to the second.");
    module.def("galaxy_collision_relative_velocity", &ic::galaxy_collision_relative_velocity,
               "parameters"_a, "The velocity of the second galaxy relative to the first.");
    module.def("galaxy_collision_orbit_energy", &ic::galaxy_collision_orbit_energy, "parameters"_a,
               "The energy of the two-body orbit the centres are on. Negative is bound.");
}

} // namespace

void bind_initial_conditions(py::module_& module) {
    bind_random(module);
    bind_plummer(module);
    bind_uniform_sphere(module);
    bind_kepler(module);
    bind_disc_galaxy(module);

    module.def(
        "move_to_centre_of_mass_frame",
        [](ParticleData& data) { ic::move_to_centre_of_mass_frame(data); }, "particles"_a,
        R"(Recentre a configuration on its centre of mass and bring it to rest.

Modifies the store in place. Every sampler here has already done it, so this is
for a configuration assembled by hand.)");
}

} // namespace orrery::python
