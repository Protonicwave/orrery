"""What an accepted submission becomes."""

from __future__ import annotations

from orrery_service.configuration import read_configuration
from orrery_service.plan import DIAGNOSTICS_FILE, TRAJECTORY_FILE, plan

CLUSTER = """
[run]
timestep = 0.001
steps = 4000
seed = 1

[initial_conditions]
kind = plummer
count = 4096
"""


def planned(text: str = CLUSTER):
    return plan(read_configuration(text))


def test_the_service_decides_the_output() -> None:
    output = planned().configuration.output
    assert output.trajectory_path == TRAJECTORY_FILE
    assert output.diagnostics_path == DIAGNOSTICS_FILE
    assert output.trajectory_velocities is False
    # A submitted run is never resumed, so nothing writes a checkpoint.
    assert output.checkpoint_path == ""
    assert output.checkpoint_stride == 0


def test_the_strides_give_about_four_hundred_frames_and_a_hundred_samples() -> None:
    result = planned()
    assert 380 <= result.frames <= 420
    assert 95 <= result.samples <= 105


def test_a_short_run_records_every_step_rather_than_none() -> None:
    # A stride of zero means the two ends of the run and nothing between them,
    # which is not what a run being watched wants.
    result = planned(CLUSTER.replace("steps = 4000", "steps = 10"))
    assert result.stride == 1
    assert result.frames == 11


def test_the_particle_count_is_what_will_be_integrated() -> None:
    assert planned().particles == 4096
    kepler = "[run]\ntimestep=1\nsteps=100\n[initial_conditions]\nkind=kepler\n"
    assert planned(kepler).particles == 2


def test_two_spellings_of_the_same_run_are_one_job() -> None:
    # The hash is taken over the settled configuration, so comments, whitespace
    # and the order of the sections do not make a second job of a run that has
    # already been taken.
    spaced = """
# The same run, written differently.

[initial_conditions]
count    = 4096
kind     = plummer

[run]
seed     = 1
steps    = 4000
timestep = 0.001
"""
    assert planned().content_hash == planned(spaced).content_hash


def test_a_different_run_is_a_different_job() -> None:
    other = CLUSTER.replace("seed = 1", "seed = 2")
    assert planned().content_hash != planned(other).content_hash


def test_the_text_is_what_the_binary_will_read() -> None:
    result = planned()
    assert read_configuration(result.text) == result.configuration
