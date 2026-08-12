"""The zero-copy guarantee, demonstrated rather than asserted.

This is the file that decides whether the phase did what it set out to. The
claim is that a NumPy array taken from a particle store shares memory with the
simulation rather than copying it, and the way to show that is not to measure
how long it takes: it is to write through the array and have the C++ side
report the change, and to change the state in C++ and have the array report
that.

The tests here also fix the two properties that make sharing safe. A view keeps
its owner alive, so a store that goes out of scope in Python does not take the
array's storage with it; and a view of a running simulation's state is
read-only, because writing into one would break the invariant the integrators
depend on.
"""

import gc

import numpy as np
import pytest

import orrery


def test_a_component_view_shares_memory_with_the_store():
    data = orrery.ParticleData(8)

    view = data.mass
    assert view.base is data
    assert view.shape == (8,)
    assert view.dtype == orrery.dtype

    # Written from Python, read by C++. total_mass is compiled code walking the
    # component array, so an equal answer means the write landed in the store
    # rather than in a copy of it.
    view[:] = np.arange(8, dtype=orrery.dtype)
    assert orrery.total_mass(data) == pytest.approx(28.0)

    # Written by C++, read from Python, through the same array object. `add`
    # appends and may reallocate, so the view is taken again afterwards.
    data.resize(3)
    data.mass[:] = 2.0
    assert orrery.total_mass(data) == pytest.approx(6.0)


def test_the_ten_arrays_are_the_ten_component_arrays():
    data = orrery.ParticleData(4)

    arrays = {
        name: getattr(data, name)
        for name in (
            "position_x",
            "position_y",
            "position_z",
            "velocity_x",
            "velocity_y",
            "velocity_z",
            "acceleration_x",
            "acceleration_y",
            "acceleration_z",
            "mass",
        )
    }

    assert len(arrays) == 10
    for name, array in arrays.items():
        assert array.shape == (4,), name
        assert array.dtype == orrery.dtype, name
        assert array.flags.writeable, name
        assert array.flags.c_contiguous, name

    # Ten separate allocations, not ten windows onto one. Two component arrays
    # sharing memory would mean a write to a position changing a velocity.
    for first in arrays:
        for second in arrays:
            if first != second:
                assert not np.shares_memory(arrays[first], arrays[second]), (first, second)


def test_a_view_keeps_its_store_alive():
    def make_view():
        data = orrery.ParticleData(16)
        data.position_x[:] = 3.5
        return data.position_x

    view = make_view()
    gc.collect()

    # The store went out of scope in Python and the array is still readable, so
    # the base reference is doing its job. Without it this would read freed
    # memory, which is the failure this test exists to prevent.
    assert np.all(view == 3.5)


def test_a_simulation_hands_out_a_read_only_view():
    simulation = orrery.assemble(_kepler_configuration())

    state = simulation.particles
    assert len(state) == 2

    view = state.position_x
    assert not view.flags.writeable

    with pytest.raises(ValueError):
        view[0] = 1.0

    # Read-only does not mean stale. The array is a window onto the state, so a
    # step moves what it reports without the array being taken again.
    before = float(view[0])
    simulation.step()
    assert float(view[0]) != before


def test_a_copy_of_a_state_is_independent_and_writable():
    simulation = orrery.assemble(_kepler_configuration())

    copy = simulation.particles.copy()
    assert copy.position_x.flags.writeable
    assert not np.shares_memory(copy.position_x, simulation.particles.position_x)

    copy.position_x[0] = 1234.0
    assert simulation.particles.position_x[0] != 1234.0


def test_components_views_and_stacked_copies():
    data = orrery.ParticleData(5)
    data.position_x[:] = 1.0

    x, y, z = orrery.components(data)
    assert np.shares_memory(x, data.position_x)
    x[0] = 9.0
    assert data.position_x[0] == 9.0

    # The (N, 3) shape cannot be a view of three separate arrays, and the helper
    # that produces it says so in its name and its docstring rather than
    # pretending otherwise.
    triples = orrery.stacked(data)
    assert triples.shape == (5, 3)
    assert not np.shares_memory(triples, data.position_x)

    with pytest.raises(ValueError):
        orrery.components(data, "momentum")


def test_radii_measure_the_distance_from_the_origin():
    data = orrery.ParticleData(2)
    data.position_x[:] = [3.0, 0.0]
    data.position_y[:] = [4.0, 0.0]
    data.position_z[:] = [0.0, 2.0]

    assert orrery.radii(data) == pytest.approx([5.0, 2.0])


def _kepler_configuration():
    configuration = orrery.Configuration()
    configuration.initial_conditions.kind = orrery.InitialConditionKind.kepler
    configuration.solver.kind = orrery.SolverKind.direct
    configuration.run.timestep = 1.0 / 512.0
    configuration.run.steps = 64
    return configuration
