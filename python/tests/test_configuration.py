"""The configuration record, and its round trip through a document.

A run in this project is reproducible from a document plus a revision of the
repository. That property is worth nothing from Python unless the configuration
a script builds and the configuration a file parses are the same object, so
these tests write one out, read it back, and require the two to be equal.
"""

import pytest

import orrery


def test_a_configuration_written_out_reads_back_as_itself():
    configuration = orrery.Configuration()
    configuration.initial_conditions.kind = orrery.InitialConditionKind.disc_galaxy
    configuration.initial_conditions.count = 20000
    configuration.initial_conditions.inclination = 0.35
    configuration.run.timestep = 1.0 / 256.0
    configuration.run.steps = 6000
    configuration.run.seed = 12345
    configuration.solver.kind = orrery.SolverKind.barnes_hut
    configuration.solver.opening_angle = 0.4
    configuration.solver.quadrupole = True
    configuration.integrator.kind = orrery.IntegratorKind.yoshida4
    configuration.output.trajectory_path = "run.otj"
    configuration.output.trajectory_stride = 10

    document = orrery.write_configuration(configuration)
    assert orrery.parse_configuration(document, "test") == configuration


def test_the_python_spellings_and_the_file_spellings_agree():
    # The file uses hyphens, which an identifier cannot hold, so the Python
    # names use underscores. Anything that writes a value into a document has to
    # be able to get from one to the other, which is what str() is for.
    pairs = [
        (orrery.SolverKind.barnes_hut, "barnes-hut", orrery.parse_solver_kind),
        (orrery.SolverKind.sycl_tree, "sycl-tree", orrery.parse_solver_kind),
        (orrery.IntegratorKind.velocity_verlet, "velocity-verlet", orrery.parse_integrator_kind),
        (orrery.IntegratorKind.rk4, "rk4", orrery.parse_integrator_kind),
        (
            orrery.InitialConditionKind.galaxy_collision,
            "galaxy-collision",
            orrery.parse_initial_condition_kind,
        ),
        (orrery.ExecutorKind.work_stealing, "work-stealing", orrery.parse_executor_kind),
    ]

    for value, spelling, parse in pairs:
        assert str(value) == spelling
        assert parse(spelling) == value

    assert orrery.parse_solver_kind("no-such-solver") is None


def test_the_objections_to_a_configuration_are_reported_all_at_once():
    configuration = orrery.Configuration()

    # Nothing has been set, so the timestep and the step count are both zero and
    # both are wrong. A person who has written a document with three mistakes in
    # it should be told about three mistakes rather than made to discover them
    # one run at a time.
    problems = orrery.problems_with(configuration)
    assert len(problems) >= 2
    assert any("timestep" in problem for problem in problems)

    configuration.initial_conditions.kind = orrery.InitialConditionKind.plummer
    configuration.initial_conditions.count = 100
    configuration.run.timestep = 0.01
    configuration.run.steps = 10
    assert orrery.problems_with(configuration) == []


def test_settings_can_be_overridden_the_way_the_command_line_does():
    configuration = orrery.Configuration()
    configuration.initial_conditions.kind = orrery.InitialConditionKind.plummer
    configuration.initial_conditions.count = 100
    configuration.run.timestep = 0.01
    configuration.run.steps = 10

    orrery.apply_settings(
        configuration, ["solver.kind=direct", "run.steps=250", "solver.softening=0.02"]
    )

    assert configuration.solver.kind == orrery.SolverKind.direct
    assert configuration.run.steps == 250
    assert configuration.solver.softening == pytest.approx(0.02)

    # A setting that names nothing is an error rather than a line quietly
    # dropped, for the reason ADR-0031 gives: a run whose misspelt softening was
    # ignored is not a run that failed but one that answered a question nobody
    # asked.
    with pytest.raises(Exception):
        orrery.apply_settings(configuration, ["solver.softenning=0.02"])


def test_a_malformed_document_names_the_line_it_failed_on():
    document = "[run]\ntimestep = 0.01\nsteps = not-a-number\n"

    with pytest.raises(Exception) as failure:
        orrery.parse_configuration(document, "example.orrery")

    assert "example.orrery" in str(failure.value)


def test_an_example_from_the_repository_parses():
    # The examples are documents the command-line program runs as they are, and
    # a binding that could not read them would be reading a different language.
    document = """
[run]
timestep = 0.00390625
steps = 1000
seed = 20250101

[initial_conditions]
kind = plummer
count = 4096

[solver]
kind = barnes-hut
softening = 0.02
"""
    configuration = orrery.parse_configuration(document, "inline")

    assert configuration.initial_conditions.count == 4096
    assert configuration.solver.kind == orrery.SolverKind.barnes_hut
    assert orrery.problems_with(configuration) == []


def test_the_settings_records_compare_by_value():
    first = orrery.RunSettings(timestep=0.5, steps=10, seed=3)
    second = orrery.RunSettings(timestep=0.5, steps=10, seed=3)

    assert first == second
    second.seed = 4
    assert first != second

    assert orrery.Configuration() == orrery.Configuration()
