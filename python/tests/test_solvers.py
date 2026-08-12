"""The approximation, measured against the reference it approximates.

The direct solver computes every pair and is what everything else in this
project is judged against. `compute_accelerations` is the binding that makes
that comparison available from Python: the same particles, evaluated twice,
differing only in the solver named.
"""

import numpy as np
import pytest

import orrery

SEED = 20260812


def solver_configuration(kind, opening_angle=0.5, softening=0.02):
    configuration = orrery.Configuration()
    configuration.solver.kind = kind
    configuration.solver.opening_angle = opening_angle
    configuration.solver.softening = softening
    return configuration


def sampled_sphere(count=4096):
    return orrery.plummer_sphere(orrery.PlummerParameters(count=count), seed=SEED)


def accelerations_under(kind, particles, opening_angle=0.5):
    data = particles.copy()
    count = orrery.compute_accelerations(solver_configuration(kind, opening_angle), data)
    return orrery.stacked(data, "acceleration"), count


def test_the_tree_agrees_with_direct_summation_to_its_stated_error():
    particles = sampled_sphere()

    exact, direct_count = accelerations_under(orrery.SolverKind.direct, particles)
    approximate, tree_count = accelerations_under(orrery.SolverKind.barnes_hut, particles)

    scale = np.sqrt(np.mean(np.sum(exact**2, axis=1)))
    error = np.sqrt(np.mean(np.sum((approximate - exact) ** 2, axis=1))) / scale

    # At the default opening angle of 0.5 the root mean square relative error is
    # a part in a few hundred. The bound is loose because this is a different
    # configuration from the one the published figure was taken on; what is
    # being asserted is the order of magnitude and that the two solvers are
    # computing the same physics.
    assert error < 1e-2, f"seed {SEED}, rms error {error}"

    # Both solvers evaluated once, and the tree did less work getting there.
    assert direct_count.evaluations == 1
    assert tree_count.evaluations == 1
    assert tree_count.particle_cell > 0
    assert direct_count.particle_cell == 0
    assert (
        tree_count.particle_particle + tree_count.particle_cell
        < direct_count.particle_particle
    )


def test_closing_the_opening_angle_buys_accuracy():
    particles = sampled_sphere(2048)
    exact, _ = accelerations_under(orrery.SolverKind.direct, particles)

    def rms_error(opening_angle):
        approximate, _ = accelerations_under(
            orrery.SolverKind.barnes_hut, particles, opening_angle
        )
        return np.sqrt(np.mean(np.sum((approximate - exact) ** 2, axis=1)))

    wide = rms_error(0.7)
    narrow = rms_error(0.2)

    # The opening angle is the accuracy knob, and it has to work in the
    # direction it claims: a narrower angle opens more cells and gets closer.
    assert narrow < wide, f"0.2 gave {narrow}, 0.7 gave {wide}"


def test_the_accelerations_are_written_into_the_arrays_that_were_passed():
    particles = sampled_sphere(512)

    before = particles.acceleration_x.copy()
    assert np.all(before == 0.0)

    orrery.compute_accelerations(solver_configuration(orrery.SolverKind.direct), particles)

    # In place, into the store the caller owns, which is what makes this usable
    # on a state that is a hundred megabytes.
    assert np.any(particles.acceleration_x != 0.0)


def test_a_two_body_acceleration_is_the_analytic_one():
    data = orrery.ParticleData()
    data.add((0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 2.0)
    data.add((4.0, 0.0, 0.0), (0.0, 0.0, 0.0), 3.0)

    orrery.compute_accelerations(
        solver_configuration(orrery.SolverKind.direct, softening=0.0), data
    )

    # G m / r^2 towards the other body, with G one. Powers of two throughout, so
    # the arithmetic is exact and this can be compared for equality rather than
    # against a tolerance.
    assert data.acceleration_x[0] == 3.0 / 16.0
    assert data.acceleration_x[1] == -2.0 / 16.0
    assert data.acceleration_y == pytest.approx([0.0, 0.0])


def test_the_executor_choice_does_not_change_the_answer():
    particles = sampled_sphere(2048)

    def with_executor(executor):
        configuration = solver_configuration(orrery.SolverKind.direct)
        configuration.solver.executor = executor
        data = particles.copy()
        orrery.compute_accelerations(configuration, data)
        return orrery.stacked(data, "acceleration")

    serial = with_executor(orrery.ExecutorKind.serial)
    stealing = with_executor(orrery.ExecutorKind.work_stealing)

    # Each target reads every source and writes only its own acceleration, so a
    # threaded evaluation is bit for bit identical to a serial one whatever the
    # thread count. Equality, not a tolerance.
    assert np.array_equal(serial, stealing)
