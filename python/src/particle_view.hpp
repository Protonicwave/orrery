#pragma once

/// \file
/// What a Python caller sees when it asks a running simulation for its state.
///
/// `Simulation::particles` hands out a const reference, deliberately: the
/// integrators require that the acceleration array holds the acceleration at
/// the current positions on entry to every step (ADR-0013), and a caller that
/// moved a particle between steps would break that invariant silently and get
/// one step of wrong physics for it. The C++ interface prevents that by giving
/// out no way to write.
///
/// A binding that returned the `ParticleData` class here would throw that away,
/// because a Python object carries no constness: the same ten writable array
/// properties would appear on a simulation's state as on a store the caller
/// built itself. So there are two types rather than one. This one holds a
/// pointer to state it does not own and hands out read-only views of it, and
/// the way to put a state into a simulation is `Simulation.restore`, which
/// re-establishes the invariant as part of accepting it.
///
/// The Python object that owns the storage is held rather than the storage
/// itself, so that a view outlives the expression that produced it: in
/// `simulation.particles.position_x`, the intermediate is discarded before the
/// array is used, and it is the reference to the simulation that keeps the
/// memory alive.

#include <utility>

#include <pybind11/pybind11.h>

#include "orrery/core/particle_data.hpp"

namespace orrery::python {

/// A read-only reference to a particle store owned by something else.
class ParticleView {
public:
    /// `owner` must be the Python object that owns `data`, not a wrapper around
    /// it, since it is what the NumPy views name as their base.
    ParticleView(const core::ParticleData& data, pybind11::object owner)
        : data_(&data), owner_(std::move(owner)) {}

    [[nodiscard]] const core::ParticleData& data() const noexcept { return *data_; }

    [[nodiscard]] const pybind11::object& owner() const noexcept { return owner_; }

private:
    const core::ParticleData* data_;
    pybind11::object owner_;
};

} // namespace orrery::python
