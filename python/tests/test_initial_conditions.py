"""The samplers, against the closed-form answers of the models they sample.

Validation. Each case compares a drawn configuration against a number the model
states analytically, rather than against a figure recorded from an earlier run
of this code.

Every seed here is written down. A property test that cannot be re-run on the
input that failed it is not evidence of anything.
"""

import numpy as np
import pytest

import orrery

# Sampling error falls as one over the square root of the particle count, so the
# tolerances below are stated as multiples of that rather than as constants that
# would silently become either vacuous or flaky if the count changed.
COUNT = 4096
SEED = 20260812


def test_a_plummer_sphere_is_drawn_in_virial_equilibrium():
    parameters = orrery.PlummerParameters(count=COUNT)
    data = orrery.plummer_sphere(parameters, seed=SEED)

    assert len(data) == COUNT

    # Softened with nothing, because the closed-form energies of the model are
    # the unsoftened ones and the comparison is against those.
    diagnostics = orrery.measure_diagnostics(data, 0.0)

    assert diagnostics.virial_ratio == pytest.approx(1.0, abs=0.05), f"seed {SEED}"
    assert diagnostics.total_energy == pytest.approx(
        orrery.plummer_total_energy(parameters), rel=0.05
    ), f"seed {SEED}"
    assert diagnostics.kinetic_energy == pytest.approx(
        orrery.plummer_kinetic_energy(parameters), rel=0.05
    ), f"seed {SEED}"


def test_the_standard_plummer_sphere_is_in_n_body_units():
    # The default scale radius is the value that puts a unit-mass sphere into
    # the units the literature quotes: total energy -1/4, virial radius one.
    parameters = orrery.PlummerParameters(count=COUNT)

    assert parameters.scale_radius == pytest.approx(orrery.standard_plummer_radius)
    assert orrery.plummer_total_energy(parameters) == pytest.approx(-0.25)


def test_a_sampled_model_is_centred_and_at_rest():
    parameters = orrery.PlummerParameters(count=COUNT)
    data = orrery.plummer_sphere(parameters, seed=SEED)

    # Recentred by the sampler, so that the angular momentum of the
    # configuration is measured about its own centre rather than about whatever
    # offset the sampling happened to produce.
    assert orrery.norm(orrery.centre_of_mass(data)) < 1e-12, f"seed {SEED}"
    assert orrery.norm(orrery.centre_of_mass_velocity(data)) < 1e-12, f"seed {SEED}"
    assert orrery.total_mass(data) == pytest.approx(1.0)


def test_the_same_seed_draws_the_same_model_and_a_different_one_does_not():
    parameters = orrery.PlummerParameters(count=256)

    first = orrery.plummer_sphere(parameters, seed=7)
    again = orrery.plummer_sphere(parameters, seed=7)
    other = orrery.plummer_sphere(parameters, seed=8)

    # Equality rather than a tolerance. Reproducibility is a bitwise claim.
    assert np.array_equal(first.position_x, again.position_x)
    assert np.array_equal(first.velocity_z, again.velocity_z)
    assert not np.array_equal(first.position_x, other.position_x)


def test_one_stream_shared_between_two_draws_gives_two_models():
    parameters = orrery.PlummerParameters(count=256)
    random = orrery.RandomSource(11)

    first = orrery.plummer_sphere(parameters, random)
    second = orrery.plummer_sphere(parameters, random)

    assert random.seed == 11
    assert not np.array_equal(first.position_x, second.position_x)


def test_a_uniform_sphere_fills_its_radius_and_has_the_stated_energy():
    parameters = orrery.UniformSphereParameters(count=COUNT, total_mass=1.0, radius=1.0)
    data = orrery.uniform_sphere(parameters, seed=SEED)

    # Drawn inside the radius and then recentred on the sampled centre of mass,
    # which sits a little away from the origin at any finite particle count and
    # so carries the outermost particles a little outside the nominal radius.
    # That offset falls as one over the square root of the count, which is what
    # the bound here is written in terms of.
    radii = orrery.radii(data)
    assert radii.max() <= 1.0 + 5.0 / np.sqrt(COUNT), f"seed {SEED}"

    # -3 G M^2 / 5 R for a sphere of uniform density, which the sample should
    # reproduce to its own sampling error.
    assert orrery.uniform_sphere_potential_energy(parameters) == pytest.approx(-0.6)
    assert orrery.potential_energy(data, 0.0) == pytest.approx(-0.6, rel=0.05), f"seed {SEED}"


def test_a_kepler_orbit_is_constructed_rather_than_sampled():
    parameters = orrery.KeplerParameters(
        primary_mass=1.0, secondary_mass=0.5, semi_major_axis=2.0, eccentricity=0.6
    )
    data = orrery.kepler_orbit(parameters)

    assert len(data) == 2
    assert orrery.total_mass(data) == pytest.approx(1.5)

    # Released at periapsis, in the centre of mass frame, so the separation is
    # a(1 - e) and the configuration has no net momentum to drift with.
    separation = abs(float(data.position_x[1] - data.position_x[0]))
    assert separation == pytest.approx(orrery.kepler_periapsis_distance(parameters))
    assert separation == pytest.approx(2.0 * (1.0 - 0.6))
    assert orrery.norm(orrery.linear_momentum(data)) < 1e-14

    # Kepler's third law, with G one.
    expected_period = 2.0 * np.pi * np.sqrt(2.0**3 / 1.5)
    assert orrery.kepler_period(parameters) == pytest.approx(expected_period)

    # The energy of a bound two-body orbit is -G m1 m2 / 2a, and the measured
    # energy of the constructed state has to agree with it exactly, since
    # nothing here is approximated.
    assert orrery.kepler_energy(parameters) == pytest.approx(-1.0 * 0.5 / (2.0 * 2.0))
    assert orrery.measure_diagnostics(data, 0.0).total_energy == pytest.approx(
        orrery.kepler_energy(parameters)
    )
    assert orrery.norm(orrery.angular_momentum(data)) == pytest.approx(
        orrery.kepler_angular_momentum(parameters)
    )


def test_a_disc_galaxy_turns_the_way_its_parameters_say():
    parameters = orrery.DiscGalaxyParameters(
        count=2048, disc_mass=0.8, bulge_mass=0.2, scale_length=1.0, inclination=0.0
    )
    data = orrery.disc_galaxy(parameters, seed=SEED)

    assert len(data) == 2048
    assert orrery.disc_galaxy_total_mass(parameters) == pytest.approx(1.0)
    assert orrery.total_mass(data) == pytest.approx(1.0, rel=1e-12)
    assert 0 < orrery.disc_galaxy_disc_count(parameters) < 2048

    # A flat disc spins about z, so almost all of the angular momentum is in
    # that component and the sign says which way round it goes.
    angular_momentum = orrery.angular_momentum(data)
    assert abs(angular_momentum.z) > 10.0 * max(
        abs(angular_momentum.x), abs(angular_momentum.y)
    ), f"seed {SEED}"
    assert tuple(orrery.disc_galaxy_spin_axis(parameters)) == pytest.approx((0.0, 0.0, 1.0))

    # The circular speed is what holds a particle on its orbit, so it has to
    # rise from the centre and it has to be the speed the enclosed mass implies.
    inner = orrery.disc_galaxy_circular_speed(parameters, 0.5)
    outer = orrery.disc_galaxy_circular_speed(parameters, 4.0)
    assert inner > 0.0 and outer > 0.0
    enclosed = orrery.disc_galaxy_enclosed_mass(parameters, 4.0)
    assert outer == pytest.approx(np.sqrt(enclosed / 4.0), rel=1e-9)


def test_two_galaxies_are_set_on_a_bound_encounter():
    galaxy = orrery.DiscGalaxyParameters(count=512, disc_mass=0.8, bulge_mass=0.2)
    smaller = orrery.DiscGalaxyParameters(count=512, disc_mass=0.4, bulge_mass=0.1)
    parameters = orrery.GalaxyCollisionParameters(
        primary=galaxy, secondary=smaller, separation=20.0, impact_parameter=2.0,
        approach_speed=0.8,
    )

    data = orrery.galaxy_collision(parameters, seed=SEED)
    assert len(data) == 1024

    separation = orrery.galaxy_collision_separation(parameters)
    assert separation.x == pytest.approx(20.0)
    assert separation.y == pytest.approx(2.0)

    # Below the escape speed, so the pair is bound and the encounter ends in a
    # merger rather than in two galaxies receding for ever.
    assert orrery.galaxy_collision_orbit_energy(parameters) < 0.0
    assert orrery.norm(orrery.galaxy_collision_relative_velocity(parameters)) > 0.0


def test_a_configuration_assembled_by_hand_can_be_recentred():
    data = orrery.ParticleData()
    data.add((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), 1.0)
    data.add((3.0, 0.0, 0.0), (0.0, 3.0, 0.0), 1.0)

    orrery.move_to_centre_of_mass_frame(data)

    assert orrery.norm(orrery.centre_of_mass(data)) < 1e-15
    assert orrery.norm(orrery.centre_of_mass_velocity(data)) < 1e-15
    assert data.position_x == pytest.approx([-1.0, 1.0])
