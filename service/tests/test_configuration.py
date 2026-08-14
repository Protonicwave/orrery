"""The reader, against the format `docs/formats/configuration.md` specifies.

Every case here is a sentence from that document. What this file cannot check is
that the C++ reader agrees, and `test_agreement.py` is where that is checked
against the binary itself.
"""

from __future__ import annotations

import pytest

from orrery_service.configuration import (
    Configuration,
    ConfigurationError,
    read_configuration,
    resolved_scale_radius,
    sections_present,
    write_configuration,
)

MINIMAL = """
[run]
timestep = 0.001
steps = 1000
"""


def test_reads_the_example_from_the_format_document() -> None:
    text = """
# A Plummer sphere of four thousand particles.

[run]
timestep = 0.001
steps    = 1000
seed     = 20260811

[initial_conditions]
kind  = plummer
count = 4096

[solver]
kind      = barnes-hut
softening = 0.05
"""
    configuration = read_configuration(text)
    assert configuration.run.timestep == 0.001
    assert configuration.run.steps == 1000
    assert configuration.run.seed == 20260811
    assert configuration.initial_conditions.kind == "plummer"
    assert configuration.initial_conditions.count == 4096
    assert configuration.solver.softening == 0.05


def test_a_setting_left_out_keeps_its_default() -> None:
    configuration = read_configuration(MINIMAL)
    assert configuration.solver.kind == "barnes-hut"
    assert configuration.integrator.kind == "velocity-verlet"
    assert configuration.initial_conditions.mass_fraction_cutoff == 0.999
    assert configuration.solver.leaf_capacity == 32
    assert configuration.solver.allow_cpu_fallback is True


def test_a_key_may_name_its_own_section() -> None:
    text = (
        "solver.softening = 0.05\n[run]\ntimestep = 1\ninitial_conditions.count = 8\n"
    )
    configuration = read_configuration(text)
    assert configuration.solver.softening == 0.05
    assert configuration.initial_conditions.count == 8
    # Naming a section does not change the one the following lines belong to.
    assert configuration.run.timestep == 1


def test_a_hash_inside_a_value_is_an_ordinary_character() -> None:
    configuration = read_configuration("[output]\ntrajectory_path = a#b.otj\n")
    assert configuration.output.trajectory_path == "a#b.otj"


@pytest.mark.parametrize(
    ("text", "because"),
    [
        ("[nonsense]\n", "an unknown section"),
        ("[run]\nsoftenning = 1\n", "an unknown setting"),
        ("[run]\ntimestep = 0.5 and a bit\n", "a number with something after it"),
        ("[run]\ntimestep = 1.0e\n", "an incomplete exponent"),
        ("[run]\nsteps = -1\n", "a whole number is not negative"),
        ("[run]\nsteps = 1_000\n", "a digit separator is not part of the format"),
        ("[solver]\nquadrupole = yes\n", "a boolean is true or false"),
        ("[solver]\nkind = octree\n", "a kind that names nothing"),
        ("[run]\ntimestep =\n", "a setting with no value"),
        ("[run]\ntimestep\n", "a line that is not a setting"),
        ("[run]\ntimestep = 1\ntimestep = 2\n", "the same setting twice"),
        ("[run]\ntimestep = 1\nrun.timestep = 2\n", "twice in the other spelling"),
        ("timestep = 1\n", "a setting outside any section"),
    ],
)
def test_refuses(text: str, because: str) -> None:
    with pytest.raises(ConfigurationError):
        read_configuration(text)


def test_reports_the_line() -> None:
    with pytest.raises(ConfigurationError) as raised:
        read_configuration("[run]\ntimestep = 1\nsteps = ten\n")
    assert raised.value.line == 3


def test_carriage_returns_do_not_reach_the_value() -> None:
    # A file written on Windows and read on Linux. Without this the last value
    # on every line would end in a character nothing on screen shows.
    configuration = read_configuration("[run]\r\ntimestep = 0.5\r\nsteps = 10\r\n")
    assert configuration.run.timestep == 0.5
    assert configuration.run.steps == 10


def test_infinities_parse_and_are_left_for_the_validator() -> None:
    # The C++ reads these through a stream, which accepts them. Refusing them
    # here would put the two readers out of step; `problems_with` is what
    # objects, and it objects because every check is the negation of what it
    # wants.
    configuration = read_configuration("[run]\ntimestep = nan\n")
    assert configuration.run.timestep != configuration.run.timestep


def test_writes_a_file_that_reads_back_the_same() -> None:
    original = read_configuration(
        "[run]\ntimestep = 0.001\nsteps = 1000\n[initial_conditions]\n"
        "kind = galaxy-collision\ncount = 8000\napproach_speed = 0.75\n"
    )
    assert read_configuration(write_configuration(original)) == original


def test_writes_every_setting() -> None:
    # What the worker hands the binary states everything, so that the run does
    # not depend on the defaults of the build reading it.
    text = write_configuration(Configuration())
    assert "allow_cpu_fallback = true" in text
    assert "mass_fraction_cutoff = 0.999" in text
    assert "leaf_capacity = 32" in text


def test_whole_numbers_are_written_without_a_point() -> None:
    text = write_configuration(read_configuration("[run]\ntimestep = 1\nsteps = 10\n"))
    assert "timestep = 1\n" in text
    assert "separation = 20\n" in text


def test_sections_present_reports_what_is_set_rather_than_what_is_named() -> None:
    assert sections_present("[output]\n") == set()
    assert sections_present("[output]\ntrajectory_path = a.otj\n") == {"output"}
    assert sections_present("[run]\noutput.trajectory_stride = 5\n") == {"output"}


def test_a_scale_radius_of_zero_is_the_standard_value() -> None:
    settings = read_configuration("[run]\ntimestep=1\n").initial_conditions
    assert resolved_scale_radius(settings) == pytest.approx(3.0 * 3.14159265 / 16.0)
