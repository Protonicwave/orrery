"""What the service accepts, and what it refuses and why.

Two sets of cases, kept apart the way the module keeps them apart: the ones the
C++ would also refuse, and the ones only this deployment refuses.
"""

from __future__ import annotations

import pytest

from orrery_service import limits
from orrery_service.configuration import read_configuration
from orrery_service.validation import (
    check_submission,
    particle_count,
    problems_with,
    service_problems,
)

CLUSTER = """
[run]
timestep = 0.001
steps = 1000
seed = 1

[initial_conditions]
kind = plummer
count = 4096

[solver]
kind = barnes-hut
softening = 0.05
"""


def settings(problems: list) -> list[str]:
    return [problem.setting for problem in problems]


def test_a_run_the_binary_would_take_has_no_problems() -> None:
    assert problems_with(read_configuration(CLUSTER)) == []


@pytest.mark.parametrize(
    ("text", "setting"),
    [
        ("[run]\nsteps = 10\n", "run.timestep"),
        ("[run]\ntimestep = -1\nsteps = 10\n", "run.timestep"),
        ("[run]\ntimestep = nan\nsteps = 10\n", "run.timestep"),
        ("[run]\ntimestep = 1\n", "run.steps"),
        (
            "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\ncount=1\n",
            "initial_conditions.count",
        ),
        (
            "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\ncount=4\ntotal_mass=0\n",
            "initial_conditions.total_mass",
        ),
        (
            "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=uniform-sphere\ncount=4\nradius=0\n",
            "initial_conditions.radius",
        ),
        (
            "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\ncount=4\nmass_fraction_cutoff=1\n",
            "initial_conditions.mass_fraction_cutoff",
        ),
        (
            "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=kepler\neccentricity=1\n",
            "initial_conditions.eccentricity",
        ),
        (
            "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=kepler\ncount=2\n",
            "initial_conditions.count",
        ),
        (
            "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=galaxy-collision\ncount=2\n",
            "initial_conditions.count",
        ),
        (
            "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=galaxy-collision\ncount=8\nmass_ratio=2\n",
            "initial_conditions.mass_ratio",
        ),
        (
            "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=disc-galaxy\ncount=8\nscale_height=0\n",
            "initial_conditions.scale_height",
        ),
        ("[run]\ntimestep=1\nsteps=1\n[solver]\nsoftening=-1\n", "solver.softening"),
        (
            "[run]\ntimestep=1\nsteps=1\n[solver]\nopening_angle=2\n",
            "solver.opening_angle",
        ),
        (
            "[run]\ntimestep=1\nsteps=1\n[solver]\nleaf_capacity=0\n",
            "solver.leaf_capacity",
        ),
        (
            "[run]\ntimestep=1\nsteps=1\n[solver]\nkind=direct\nopening_angle=0.7\n",
            "solver.opening_angle",
        ),
        (
            "[run]\ntimestep=1\nsteps=1\n[solver]\nkind=direct\nquadrupole=true\n",
            "solver.quadrupole",
        ),
        (
            "[run]\ntimestep=1\nsteps=1\n[output]\ntrajectory_stride=5\n",
            "output.trajectory_stride",
        ),
    ],
)
def test_the_cpp_rules_are_mirrored(text: str, setting: str) -> None:
    assert setting in settings(problems_with(read_configuration(text)))


def test_every_objection_is_reported_at_once() -> None:
    # Somebody who has written three mistakes is told about three mistakes.
    problems = problems_with(read_configuration("[initial_conditions]\ncount = 1\n"))
    assert settings(problems) == [
        "run.timestep",
        "run.steps",
        "initial_conditions.count",
    ]


def test_a_trajectory_and_a_checkpoint_cannot_be_one_file() -> None:
    text = (
        "[run]\ntimestep=1\nsteps=1\n[output]\n"
        "trajectory_path = a.bin\ncheckpoint_path = a.bin\n"
    )
    assert "output.trajectory_path" in settings(problems_with(read_configuration(text)))


def test_a_kepler_run_integrates_two_bodies() -> None:
    configuration = read_configuration(
        "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=kepler\n"
    )
    assert particle_count(configuration) == 2


def test_the_service_refuses_a_submission_that_decides_where_a_run_writes() -> None:
    text = CLUSTER + "\n[output]\ntrajectory_path = mine.otj\n"
    configuration = read_configuration(text)
    assert "output" in settings(service_problems(configuration, text))


def test_the_service_refuses_a_gpu_solver_it_has_no_device_for() -> None:
    text = CLUSTER.replace("kind = barnes-hut", "kind = sycl-tree")
    problems = service_problems(read_configuration(text), text)
    assert "solver.kind" in settings(problems)
    assert "no GPU" in problems[0].complaint


def test_the_service_chooses_the_scheduler_and_the_thread_count() -> None:
    text = CLUSTER + "\nsolver.executor = serial\nsolver.threads = 4\n"
    problems = service_problems(read_configuration(text), text)
    assert settings(problems) == ["solver.executor", "solver.threads"]


def test_the_particle_ceiling_is_a_refusal_rather_than_a_clamp() -> None:
    text = CLUSTER.replace("count = 4096", f"count = {limits.MAX_PARTICLES + 1}")
    problems = service_problems(read_configuration(text), text)
    assert "initial_conditions.count" in settings(problems)


def test_the_work_ceiling_stops_a_large_run_taken_for_a_long_time() -> None:
    text = CLUSTER.replace("count = 4096", f"count = {limits.MAX_PARTICLES}").replace(
        "steps = 1000", f"steps = {limits.MAX_STEPS}"
    )
    problems = service_problems(read_configuration(text), text)
    assert "run.steps" in settings(problems)


def test_the_work_ceiling_lets_a_small_run_be_taken_for_a_long_time() -> None:
    text = CLUSTER.replace("count = 4096", "count = 1000").replace(
        "steps = 1000", f"steps = {limits.MAX_STEPS}"
    )
    assert service_problems(read_configuration(text), text) == []


def test_the_direct_solver_is_scored_on_the_curve_it_follows() -> None:
    # Every pair rather than a tree walk, so the same count buys far fewer
    # steps. A ceiling that scored both the same would understate this one.
    tree = CLUSTER.replace("count = 4096", "count = 8000")
    direct = tree.replace("kind = barnes-hut", "kind = direct")
    assert service_problems(read_configuration(tree), tree) == []
    assert "run.steps" in settings(service_problems(read_configuration(direct), direct))


def test_a_configuration_that_does_not_parse_reports_the_line() -> None:
    configuration, problems = check_submission("[run]\ntimestep = ten\n")
    assert configuration is None
    assert len(problems) == 1
    assert "line 2" in problems[0].complaint


def test_a_configuration_longer_than_the_ceiling_is_not_read_at_all() -> None:
    configuration, problems = check_submission(
        "# " + "x" * limits.MAX_CONFIGURATION_BYTES + "\n"
    )
    assert configuration is None
    assert problems[0].setting == "configuration"


def test_an_accepted_submission_comes_back_with_its_configuration() -> None:
    configuration, problems = check_submission(CLUSTER)
    assert problems == []
    assert configuration is not None
    assert configuration.initial_conditions.count == 4096
