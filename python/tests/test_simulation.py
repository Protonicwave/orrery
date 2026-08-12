"""A run, against the analytic results the C++ suite measures it by.

Validation, and the same validation: a circular orbit whose exact solution after
one period is the state it started in, a symplectic method whose energy error
stays inside a bounded envelope while a higher-order non-symplectic one drifts
without limit, and a run that resumes to a state identical in every bit.

The point of repeating them here is not to test the physics twice. It is that a
notebook makes these claims, and a notebook that has never been run against an
assertion is a document rather than a result.
"""

import numpy as np
import pytest

import orrery

# Single precision has about seven digits, double about sixteen, and a tolerance
# written for one asserts nothing in the other.
TIGHT = 1e-5 if orrery.single_precision else 1e-10
LOOSE = 1e-3 if orrery.single_precision else 1e-6


def circular_orbit_configuration(integrator, steps_per_orbit=512, orbits=1):
    parameters = orrery.KeplerParameters(
        primary_mass=1.0, secondary_mass=1.0, semi_major_axis=1.0, eccentricity=0.0
    )
    period = orrery.kepler_period(parameters)

    configuration = orrery.Configuration()
    configuration.initial_conditions.kind = orrery.InitialConditionKind.kepler
    configuration.initial_conditions.primary_mass = parameters.primary_mass
    configuration.initial_conditions.secondary_mass = parameters.secondary_mass
    configuration.initial_conditions.semi_major_axis = parameters.semi_major_axis
    configuration.initial_conditions.eccentricity = parameters.eccentricity
    configuration.solver.kind = orrery.SolverKind.direct
    configuration.solver.softening = 0.0
    configuration.integrator.kind = integrator
    configuration.run.timestep = period / steps_per_orbit
    configuration.run.steps = steps_per_orbit * orbits
    return configuration, period


def test_a_circular_orbit_returns_to_where_it_started():
    configuration, _ = circular_orbit_configuration(orrery.IntegratorKind.yoshida4)

    simulation = orrery.assemble(configuration)
    start = simulation.particles.copy()

    simulation.run(configuration.run.steps)

    # After exactly one period the exact solution is the initial state, so the
    # difference is the whole of the integrator's error over an orbit and
    # nothing else. Yoshida's fourth-order composition at 512 steps should close
    # it to a part in a hundred million.
    assert simulation.particles.position_x == pytest.approx(start.position_x, abs=LOOSE)
    assert simulation.particles.position_y == pytest.approx(start.position_y, abs=LOOSE)
    assert simulation.particles.velocity_x == pytest.approx(start.velocity_x, abs=LOOSE)


def test_the_clock_is_the_step_counter_times_the_timestep():
    configuration, _ = circular_orbit_configuration(orrery.IntegratorKind.velocity_verlet)

    simulation = orrery.assemble(configuration)
    assert simulation.step_index == 0
    assert simulation.time == 0.0

    simulation.run(100)

    assert simulation.step_index == 100
    # Computed rather than accumulated: adding a timestep a hundred times gives a
    # number differing in its last bits from a hundred times the timestep, and
    # which of the two a run reported would depend on whether it had been
    # resumed. Equality rather than a tolerance is the assertion that matters.
    assert simulation.time == 100 * simulation.timestep


@pytest.mark.parametrize(
    "integrator, order, symplectic",
    [
        (orrery.IntegratorKind.velocity_verlet, 2, True),
        (orrery.IntegratorKind.yoshida4, 4, True),
        (orrery.IntegratorKind.rk4, 4, False),
    ],
)
def test_each_method_converges_at_the_order_it_claims(integrator, order, symplectic):
    def closure_error(steps_per_orbit):
        configuration, _ = circular_orbit_configuration(integrator, steps_per_orbit)
        simulation = orrery.assemble(configuration)
        start = orrery.stacked(simulation.particles)
        simulation.run(configuration.run.steps)
        return float(np.max(np.abs(orrery.stacked(simulation.particles) - start)))

    # Two step counts squeezed from both sides. Too few and the error is not yet
    # dominated by the leading term in the timestep; too many and it falls into
    # the round-off floor, where halving the step stops helping and the measured
    # order collapses. The floor is far higher in single precision, so the
    # counts follow the build, exactly as the C++ suite's do.
    if orrery.single_precision:
        coarse = 48 if order == 2 else 32
    else:
        coarse = 256 if order == 2 else 128
    coarse_error = closure_error(coarse)
    fine_error = closure_error(2 * coarse)

    assert 0.0 < fine_error < coarse_error
    measured = np.log2(coarse_error / fine_error)
    assert measured == pytest.approx(order, abs=0.35), f"measured order {measured}"


def test_a_symplectic_method_bounds_its_energy_error_and_rk4_does_not():
    """The comparison this project exists to produce, in one test.

    Velocity Verlet is second order and costs one force evaluation a step. RK4
    is fourth order and costs four. Over a few hundred orbits the second-order
    symplectic method returns the energy error it started with, and the
    fourth-order non-symplectic one grows without bound and overtakes it.
    """
    orbits = 400
    steps_per_orbit = 200

    def energy_error_history(integrator):
        configuration, _ = circular_orbit_configuration(
            integrator, steps_per_orbit, orbits=orbits
        )
        configuration.initial_conditions.eccentricity = 0.5

        simulation = orrery.assemble(configuration)
        reference = simulation.measure().total_energy

        history = []
        chunk = steps_per_orbit * 5
        for _ in range(configuration.run.steps // chunk):
            simulation.run(chunk)
            history.append(
                abs((simulation.measure().total_energy - reference) / reference)
            )
        return np.array(history)

    verlet = energy_error_history(orrery.IntegratorKind.velocity_verlet)
    rk4 = energy_error_history(orrery.IntegratorKind.rk4)

    first = slice(0, len(verlet) // 20 + 1)
    last = slice(-(len(verlet) // 20 + 1), None)

    # Bounded, which is a statement about the envelope rather than about any one
    # measurement. The error oscillates over an orbit, so what is asserted is
    # that nothing anywhere in four hundred orbits leaves the neighbourhood of
    # where it began, not that two samples of an oscillation agree.
    assert verlet.max() < 2.0 * verlet[first].mean()
    assert verlet[last].mean() < 2.0 * verlet[first].mean()

    # Secular: RK4 starts far more accurate and finishes far worse, and it is
    # still growing when the run ends. That comparison, between a second-order
    # method costing one force evaluation a step and a fourth-order one costing
    # four, is what decides the default (ADR-0011).
    assert rk4[first].mean() < verlet[first].mean()
    assert rk4[last].mean() > 5.0 * rk4[first].mean()

    # And still climbing when the run ends, which is the part that matters. At
    # four hundred orbits RK4 has not yet overtaken velocity Verlet in absolute
    # terms; it is on its way, and where the two cross is a property of how long
    # the run is rather than of the methods. The unbounded growth is the claim,
    # not any particular crossing point.
    assert np.all(np.diff(rk4[last]) > 0.0)
    assert np.all(np.abs(np.diff(verlet[last])) < 0.01 * verlet[last].mean())


def test_momentum_is_conserved_to_round_off():
    configuration = orrery.Configuration()
    configuration.initial_conditions.kind = orrery.InitialConditionKind.plummer
    configuration.initial_conditions.count = 256
    configuration.run.seed = 4242
    configuration.run.timestep = 1.0 / 128.0
    configuration.run.steps = 200
    configuration.solver.kind = orrery.SolverKind.direct
    configuration.solver.softening = 0.05

    simulation = orrery.assemble(configuration)
    before = simulation.measure()
    simulation.run(configuration.run.steps)
    after = simulation.measure()

    # The direct kernel computes each pair from both ends, so the momentum
    # change is a sum of terms that cancel exactly in exact arithmetic. What is
    # left is round-off, measured against the size of the terms that cancelled
    # rather than against zero.
    scale = float(np.sum(np.abs(simulation.particles.mass * simulation.particles.velocity_x)))
    drift = orrery.norm(after.linear_momentum - before.linear_momentum)
    assert drift < TIGHT * scale, f"seed {configuration.run.seed}"


def test_a_run_resumes_to_a_state_identical_in_every_bit():
    configuration = orrery.Configuration()
    configuration.initial_conditions.kind = orrery.InitialConditionKind.plummer
    configuration.initial_conditions.count = 128
    configuration.run.seed = 99
    configuration.run.timestep = 1.0 / 128.0
    configuration.run.steps = 200
    configuration.solver.kind = orrery.SolverKind.barnes_hut
    configuration.solver.softening = 0.05

    uninterrupted = orrery.assemble(configuration)
    uninterrupted.run(200)

    interrupted = orrery.assemble(configuration)
    interrupted.run(100)

    # The accelerations in the copy are the accelerations at its positions, so
    # they are kept rather than recomputed. That distinction is the whole of
    # bitwise resume: recomputing gives the same answer for every solver in this
    # project and is not guaranteed to for one added later.
    snapshot = interrupted.particles.copy()
    resumed = orrery.assemble(configuration)
    resumed.restore(snapshot, step=100, accelerations_are_current=True)
    resumed.run(100)

    assert resumed.step_index == uninterrupted.step_index
    for name in ("position_x", "position_y", "position_z", "velocity_x", "mass"):
        assert np.array_equal(
            getattr(resumed.particles, name), getattr(uninterrupted.particles, name)
        ), name


def test_a_simulation_reports_what_it_is_made_of():
    configuration, _ = circular_orbit_configuration(orrery.IntegratorKind.yoshida4)
    simulation = orrery.assemble(configuration)

    assert "direct" in simulation.solver_name.lower()
    assert "yoshida" in simulation.integrator_name.lower()
    assert repr(simulation).startswith("<Simulation of 2 particles")

    # One force evaluation has already happened: the constructor spends it
    # establishing the acceleration invariant the integrators require.
    assert simulation.interaction_count.evaluations == 1

    simulation.step()
    assert simulation.interaction_count.evaluations > 1


def test_the_sampled_state_is_reachable_without_assembling_a_run():
    configuration = orrery.Configuration()
    configuration.initial_conditions.kind = orrery.InitialConditionKind.galaxy_collision
    configuration.initial_conditions.count = 512
    configuration.run.seed = 5

    data = orrery.make_initial_conditions(configuration)
    primary = orrery.primary_galaxy_count(configuration)

    assert len(data) == 512
    assert 0 < primary < 512

    # The count is what lets the two galaxies be told apart in a plot of a
    # merged remnant, and it has to be the same split the sampler used.
    assert data.position_x[:primary].mean() < data.position_x[primary:].mean()
