"""Whether a submitted configuration is a run this service will take.

Two questions, asked in order, and kept apart because they have different
authorities.

The first is the one `sim/configuration.cpp` asks: is this a run at all. Its
answers are mirrored here setting for setting, including the ones that look
pedantic, because a submission this service accepted and the binary then refused
would be a job that occupies a worker in order to fail. `problems_with` below is
that mirror and nothing else: no ceiling, no policy, nothing about this service.

The second is the one only this service can ask: is this a run it will take.
That is the ceilings in `limits.py`, the solvers the worker can actually
provide, and the settings a submission has no business deciding. Those live in
`service_problems`.

Both refuse rather than clamp, and both report every objection at once. A run
quietly reduced to what the service felt like giving would produce a picture of
a scenario nobody asked about, and one reported objection at a time would make
somebody submit four times to learn four things.
"""

from __future__ import annotations

from . import limits
from .configuration import (
    Configuration,
    ConfigurationError,
    read_configuration,
    sections_present,
)
from .contract import Problem

#: The default the C++ compares against when it reports an opening angle that a
#: direct solver will not read. Stated here so the mirror is exact.
_DEFAULT_OPENING_ANGLE = 0.5


def _is_sampled(kind: str) -> bool:
    return kind != "kepler"


def _is_galaxy(kind: str) -> bool:
    return kind in ("disc-galaxy", "galaxy-collision")


def _is_tree_solver(kind: str) -> bool:
    return kind in ("barnes-hut", "sycl-tree")


def _positive(value: float) -> bool:
    """Whether `value` is a positive number.

    Written as a function returning the condition wanted rather than as a
    comparison at each site, so that every check is the negation of this and a
    NaN fails all of them. `configuration.cpp` explains the same idea at the one
    place it matters most: `timestep <= 0` would let a NaN through into an
    integration where every position becomes NaN on the first step.
    """
    return value > 0


def _check_run(configuration: Configuration, problems: list[Problem]) -> None:
    run = configuration.run
    if not _positive(run.timestep):
        problems.append(
            Problem(setting="run.timestep", complaint="must be a positive number")
        )
    if run.steps == 0:
        problems.append(Problem(setting="run.steps", complaint="must be at least one"))


def _check_kepler(configuration: Configuration, problems: list[Problem]) -> None:
    initial = configuration.initial_conditions
    for name in ("primary_mass", "secondary_mass", "semi_major_axis"):
        if not _positive(getattr(initial, name)):
            problems.append(
                Problem(
                    setting=f"initial_conditions.{name}",
                    complaint="must be a positive number",
                )
            )
    if not (initial.eccentricity >= 0) or not (initial.eccentricity < 1):
        problems.append(
            Problem(
                setting="initial_conditions.eccentricity",
                complaint="must lie in [0, 1) for a bound orbit",
            )
        )
    if initial.count != 0:
        problems.append(
            Problem(
                setting="initial_conditions.count",
                complaint=(
                    "is not used by the kepler configuration, which is always "
                    "two bodies"
                ),
            )
        )


def _check_galaxy(configuration: Configuration, problems: list[Problem]) -> None:
    initial = configuration.initial_conditions
    if not (initial.bulge_fraction >= 0) or not (initial.bulge_fraction < 1):
        problems.append(
            Problem(
                setting="initial_conditions.bulge_fraction",
                complaint="must lie in [0, 1)",
            )
        )
    for name in ("scale_length", "scale_height", "bulge_radius"):
        if not _positive(getattr(initial, name)):
            problems.append(
                Problem(
                    setting=f"initial_conditions.{name}",
                    complaint="must be a positive number",
                )
            )


def _check_collision(configuration: Configuration, problems: list[Problem]) -> None:
    initial = configuration.initial_conditions
    if not _positive(initial.mass_ratio) or not (initial.mass_ratio <= 1):
        problems.append(
            Problem(
                setting="initial_conditions.mass_ratio",
                complaint="must lie in (0, 1]",
            )
        )
    if initial.separation == 0 and initial.impact_parameter == 0:
        problems.append(
            Problem(
                setting="initial_conditions.separation",
                complaint=(
                    "and initial_conditions.impact_parameter cannot both be "
                    "zero, since the two galaxies would start on top of one "
                    "another"
                ),
            )
        )
    if not (initial.approach_speed >= 0):
        problems.append(
            Problem(
                setting="initial_conditions.approach_speed",
                complaint="must not be negative",
            )
        )
    if initial.count < 4:
        problems.append(
            Problem(
                setting="initial_conditions.count",
                complaint=(
                    "must be at least four for a collision, which is two galaxies"
                ),
            )
        )


def _check_initial_conditions(
    configuration: Configuration, problems: list[Problem]
) -> None:
    initial = configuration.initial_conditions

    if _is_sampled(initial.kind):
        if initial.count < 2:
            problems.append(
                Problem(
                    setting="initial_conditions.count",
                    complaint="must be at least two for a sampled configuration",
                )
            )
        if not _positive(initial.total_mass):
            problems.append(
                Problem(
                    setting="initial_conditions.total_mass",
                    complaint="must be a positive number",
                )
            )

    if initial.kind == "plummer" or _is_galaxy(initial.kind):
        cutoff = initial.mass_fraction_cutoff
        if not _positive(cutoff) or not (cutoff < 1):
            problems.append(
                Problem(
                    setting="initial_conditions.mass_fraction_cutoff",
                    complaint="must lie strictly in (0, 1)",
                )
            )

    if _is_galaxy(initial.kind):
        _check_galaxy(configuration, problems)

    if initial.kind == "plummer":
        if initial.scale_radius < 0:
            problems.append(
                Problem(
                    setting="initial_conditions.scale_radius",
                    complaint=(
                        "must be positive, or zero for the standard N-body value"
                    ),
                )
            )
    elif initial.kind == "uniform-sphere":
        if not _positive(initial.radius):
            problems.append(
                Problem(
                    setting="initial_conditions.radius",
                    complaint="must be a positive number",
                )
            )
    elif initial.kind == "kepler":
        _check_kepler(configuration, problems)
    elif initial.kind == "galaxy-collision":
        _check_collision(configuration, problems)


def _check_solver(configuration: Configuration, problems: list[Problem]) -> None:
    solver = configuration.solver
    if not (solver.softening >= 0):
        problems.append(
            Problem(setting="solver.softening", complaint="must not be negative")
        )

    if _is_tree_solver(solver.kind):
        angle = solver.opening_angle
        if not (angle >= 0) or not (angle <= 1):
            problems.append(
                Problem(setting="solver.opening_angle", complaint="must lie in [0, 1]")
            )
        if solver.leaf_capacity == 0:
            problems.append(
                Problem(
                    setting="solver.leaf_capacity", complaint="must be at least one"
                )
            )
    else:
        if solver.opening_angle != _DEFAULT_OPENING_ANGLE:
            problems.append(
                Problem(
                    setting="solver.opening_angle",
                    complaint=(
                        "is not used by a direct solver, which computes every pair"
                    ),
                )
            )
        if solver.quadrupole:
            problems.append(
                Problem(
                    setting="solver.quadrupole",
                    complaint="is not used by a direct solver",
                )
            )


def _check_output(configuration: Configuration, problems: list[Problem]) -> None:
    output = configuration.output
    pairs = (
        ("trajectory", output.trajectory_path, output.trajectory_stride),
        ("diagnostics", output.diagnostics_path, output.diagnostics_stride),
        ("checkpoint", output.checkpoint_path, output.checkpoint_stride),
    )
    for name, path, stride in pairs:
        if path == "" and stride != 0:
            problems.append(
                Problem(
                    setting=f"output.{name}_stride",
                    complaint=f"has no effect without output.{name}_path",
                )
            )
    if (
        output.trajectory_path != ""
        and output.trajectory_path == output.checkpoint_path
    ):
        problems.append(
            Problem(
                setting="output.trajectory_path",
                complaint=(
                    "is the same file as output.checkpoint_path, and the two "
                    "formats differ"
                ),
            )
        )


def problems_with(configuration: Configuration) -> list[Problem]:
    """Every objection the C++ would raise, in the order the settings appear.

    The mirror of `orrery::sim::problems_with`. Empty for a configuration the
    binary would run. `tests/test_agreement.py` is what keeps the two the same
    function: it puts the same configurations through both and requires them to
    agree about which are accepted.
    """
    problems: list[Problem] = []
    _check_run(configuration, problems)
    _check_initial_conditions(configuration, problems)
    _check_solver(configuration, problems)
    _check_output(configuration, problems)
    return problems


def particle_count(configuration: Configuration) -> int:
    """How many particles the run will actually integrate.

    The count for a sampled configuration, and two for the Kepler problem, whose
    count is fixed by the physics rather than chosen. Needed by the ceilings,
    which are about what a machine has to compute rather than about what a file
    says.
    """
    initial = configuration.initial_conditions
    return initial.count if _is_sampled(initial.kind) else 2


def service_problems(configuration: Configuration, text: str) -> list[Problem]:
    """Every reason this service will not take a run the binary would.

    Separate from `problems_with` because these are properties of the service
    rather than of the configuration. A submission refused here is a valid
    document that this deployment cannot or will not run, and the message says
    which of the two it is.
    """
    problems: list[Problem] = []
    solver = configuration.solver

    # Where a run writes is the service's business, not the submitter's. The
    # worker sets the whole output section, so a submission that stated one
    # would have it silently replaced, and a setting silently replaced is the
    # thing this format's strictness exists to prevent.
    if "output" in sections_present(text):
        problems.append(
            Problem(
                setting="output",
                complaint=(
                    "is set by the service rather than by a submission. Remove "
                    "the section: the result is fetched from the job rather "
                    "than written to a path you choose"
                ),
            )
        )

    if solver.kind not in limits.ALLOWED_SOLVERS:
        problems.append(
            Problem(
                setting="solver.kind",
                complaint=(
                    f"names a solver this service cannot provide. The worker has "
                    f"no GPU, so it runs {' or '.join(limits.ALLOWED_SOLVERS)}. "
                    f"The published GPU figures were measured on the machine "
                    f"docs/performance.md names, not here"
                ),
            )
        )

    # The scheduler and the thread count are properties of the machine the run
    # lands on, and the work ceiling below is priced on the defaults. A run that
    # asked for the serial executor would cost several times its score.
    if solver.executor != "work-stealing":
        problems.append(
            Problem(
                setting="solver.executor",
                complaint=(
                    "is chosen by the service. The ceiling on a submission is "
                    "priced on the default scheduler, so a run that asked for "
                    "another one would cost more than it was allowed"
                ),
            )
        )
    if solver.threads != 0:
        problems.append(
            Problem(
                setting="solver.threads",
                complaint=(
                    "is chosen by the service, which gives a run one worker per "
                    "core of whatever machine it lands on"
                ),
            )
        )

    count = particle_count(configuration)
    steps = configuration.run.steps

    if count > limits.MAX_PARTICLES:
        problems.append(
            Problem(
                setting="initial_conditions.count",
                complaint=(
                    f"is {count}, and this service takes at most "
                    f"{limits.MAX_PARTICLES}, which is the largest count the "
                    f"repository publishes a measured step time for"
                ),
            )
        )
    if steps > limits.MAX_STEPS:
        problems.append(
            Problem(
                setting="run.steps",
                complaint=(
                    f"is {steps}, and this service takes at most {limits.MAX_STEPS}"
                ),
            )
        )

    work = limits.work_units(count, steps, solver.kind)
    if work > limits.MAX_WORK:
        problems.append(
            Problem(
                setting="run.steps",
                complaint=(
                    f"asks for {work / limits.MAX_WORK:.1f} times the work this "
                    f"service will take in one run. The ceiling is the "
                    f"demonstration in README.md: {limits.MAX_PARTICLES} "
                    f"particles over 6000 steps with the tree solver. Fewer "
                    f"particles buys more steps"
                ),
            )
        )

    return problems


def check_submission(text: str) -> tuple[Configuration | None, list[Problem]]:
    """Read a submitted configuration and say everything wrong with it.

    Returns the configuration and an empty list, or `None` and the objections. A
    file that does not parse produces one objection naming the line, because the
    rest of the settings cannot be checked when the reader stopped before
    reaching them: that is the one case where the report is not complete, and it
    is complete about the reason it is not.
    """
    if len(text.encode("utf-8")) > limits.MAX_CONFIGURATION_BYTES:
        return None, [
            Problem(
                setting="configuration",
                complaint=(
                    f"is longer than {limits.MAX_CONFIGURATION_BYTES} bytes. A "
                    f"configuration is about twenty settings"
                ),
            )
        ]

    try:
        configuration = read_configuration(text)
    except ConfigurationError as error:
        return None, [Problem(setting="configuration", complaint=str(error))]

    problems = problems_with(configuration)
    problems.extend(service_problems(configuration, text))
    if problems:
        return None, problems
    return configuration, []
