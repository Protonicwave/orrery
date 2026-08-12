"""The scalar and vector types, and the particle store's invariants.

Unit tests. The behaviour they check is the C++ library's and is already tested
there; what is being tested here is that the binding presents it, since a
property that holds in C++ and is unreachable from Python is not a property this
package has.
"""

import numpy as np
import pytest

import orrery


def test_a_vector_is_a_sequence_of_three():
    v = orrery.Vec3(1.0, 2.0, 3.0)

    assert (v.x, v.y, v.z) == (1.0, 2.0, 3.0)
    assert len(v) == 3
    assert tuple(v) == (1.0, 2.0, 3.0)
    assert list(v) == [1.0, 2.0, 3.0]
    assert np.asarray(v).tolist() == [1.0, 2.0, 3.0]

    x, y, z = v
    assert (x, y, z) == (1.0, 2.0, 3.0)

    assert v[-1] == 3.0
    with pytest.raises(IndexError):
        _ = v[3]


def test_vector_arithmetic_matches_the_definitions():
    a = orrery.Vec3(1.0, 2.0, 3.0)
    b = orrery.Vec3(4.0, 5.0, 6.0)

    assert tuple(a + b) == (5.0, 7.0, 9.0)
    assert tuple(b - a) == (3.0, 3.0, 3.0)
    assert tuple(-a) == (-1.0, -2.0, -3.0)
    assert tuple(a * 2.0) == (2.0, 4.0, 6.0)
    assert tuple(2.0 * a) == (2.0, 4.0, 6.0)
    assert tuple(a / 2.0) == (0.5, 1.0, 1.5)

    assert orrery.dot(a, b) == pytest.approx(32.0)
    assert tuple(orrery.cross(orrery.Vec3(1, 0, 0), orrery.Vec3(0, 1, 0))) == (0.0, 0.0, 1.0)
    assert orrery.norm(orrery.Vec3(3, 4, 0)) == pytest.approx(5.0)

    assert a == orrery.Vec3(1.0, 2.0, 3.0)
    assert a != b


def test_a_tuple_is_accepted_where_a_vector_is_expected():
    data = orrery.ParticleData()

    # `add` takes two vectors. Requiring Vec3 at every call site would make
    # setting up a configuration by hand read worse than the physics it states.
    index = data.add((1.0, 2.0, 3.0), (0.0, 0.0, 0.5), 1.0)

    assert index == 0
    assert data.position_x[0] == 1.0
    assert data.velocity_z[0] == 0.5

    # A sequence of the wrong length is refused rather than padded, since a
    # two-component position is a mistake and not a shorthand.
    with pytest.raises((TypeError, ValueError)):
        data.add((1.0, 2.0), (0.0, 0.0, 0.0), 1.0)


def test_softening_carries_its_square():
    softening = orrery.Softening(0.5)

    assert softening.length == pytest.approx(0.5)
    assert softening.squared == pytest.approx(0.25)
    assert orrery.Softening().squared == 0.0

    # A number where a softening is expected means a softening of that length,
    # so the diagnostics can be asked for with the value the solver was given.
    data = orrery.ParticleData(2)
    data.position_x[:] = [0.0, 1.0]
    data.mass[:] = 1.0

    assert orrery.potential_energy(data, 0.0) == pytest.approx(-1.0)
    assert orrery.potential_energy(data, orrery.Softening(0.0)) == pytest.approx(-1.0)
    assert orrery.potential_energy(data, 1.0) == pytest.approx(-1.0 / np.sqrt(2.0))


def test_the_store_keeps_its_size_invariant():
    data = orrery.ParticleData()

    assert len(data) == 0
    assert data.size == 0

    data.resize(10)
    assert data.size == 10
    assert all(len(getattr(data, name)) == 10 for name in ("position_x", "velocity_y", "mass"))

    data.reserve(100)
    assert data.capacity >= 100
    assert data.size == 10

    data.clear()
    assert data.size == 0
    assert data.capacity >= 100

    assert orrery.ParticleData(7).size == 7


def test_a_new_store_is_zero_rather_than_uninitialised():
    data = orrery.ParticleData(64)

    for name in ("position_x", "velocity_y", "acceleration_z", "mass"):
        assert np.all(getattr(data, name) == 0.0), name


def test_diagnostics_derive_their_two_ratios():
    diagnostics = orrery.Diagnostics()
    diagnostics.kinetic_energy = 0.5
    diagnostics.potential_energy = -1.0

    assert diagnostics.total_energy == pytest.approx(-0.5)
    assert diagnostics.virial_ratio == pytest.approx(1.0)

    # The convention this project quotes is 2T / |U|, which some authors write
    # as T / |U| and get a half. A test is the cheapest place to fix it.
    diagnostics.kinetic_energy = 0.25
    assert diagnostics.virial_ratio == pytest.approx(0.5)


def test_the_conserved_quantities_of_a_configuration_by_hand():
    # Two unit masses, one at each end of a unit separation, moving oppositely
    # along y. Every quantity below is exact and can be checked by hand, which
    # is the point of using this rather than a sampled model.
    data = orrery.ParticleData(2)
    data.position_x[:] = [-0.5, 0.5]
    data.velocity_y[:] = [-1.0, 1.0]
    data.mass[:] = 1.0

    assert orrery.total_mass(data) == pytest.approx(2.0)
    assert tuple(orrery.centre_of_mass(data)) == pytest.approx((0.0, 0.0, 0.0))
    assert tuple(orrery.centre_of_mass_velocity(data)) == pytest.approx((0.0, 0.0, 0.0))
    assert orrery.kinetic_energy(data) == pytest.approx(1.0)
    assert orrery.potential_energy(data, 0.0) == pytest.approx(-1.0)
    assert tuple(orrery.linear_momentum(data)) == pytest.approx((0.0, 0.0, 0.0))
    assert tuple(orrery.angular_momentum(data)) == pytest.approx((0.0, 0.0, 1.0))

    measured = orrery.measure_diagnostics(data, 0.0)
    assert measured.kinetic_energy == pytest.approx(1.0)
    assert measured.potential_energy == pytest.approx(-1.0)
    assert measured.total_energy == pytest.approx(0.0)


def test_the_build_reports_what_it_was_configured_for():
    assert orrery.__version__.count(".") >= 1
    assert isinstance(orrery.single_precision, bool)
    assert orrery.dtype == (np.float32 if orrery.single_precision else np.float64)
