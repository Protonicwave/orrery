"""The `.orrery` format, read as the C++ reads it.

`docs/formats/configuration.md` specifies the file and `src/sim/config_file.cpp`
implements it. This is a third implementation of the same specification, and
that is worth being uncomfortable about, so it is worth saying exactly why it
exists and what stops it drifting.

It exists because the service refuses a submission rather than clamping it, and
a refusal has to be able to say which setting is wrong. Handing the text
straight to the binary and reporting whatever it printed would work, but it
would mean starting a container to find out that somebody typed `softenning`,
and it would mean the queue accepting jobs that cannot run. The check has to
happen before the job is stored, which means it happens here.

What stops it drifting is `tests/test_agreement.py`. It generates configurations
across the whole settings table, runs `orrery show` over each of them, and
requires this reader and that one to agree about which are accepted and which
are refused. The test skips itself when the binary is absent and runs in
continuous integration, where it is present. A specification with three readers
and no test between them would be three specifications.

The client's `web/src/config/parse.ts` is deliberately not a fourth. It reads
the syntax and knows nothing of the settings' names, because what it needs is to
display a file the repository already contains rather than to decide whether an
unfamiliar one is a run.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field, replace
from typing import Literal

SolverKind = Literal["direct", "barnes-hut", "sycl-direct", "sycl-tree"]
IntegratorKind = Literal["velocity-verlet", "yoshida4", "rk4"]
InitialConditionKind = Literal[
    "plummer", "uniform-sphere", "kepler", "disc-galaxy", "galaxy-collision"
]
ExecutorKind = Literal["serial", "static", "work-stealing"]

SOLVER_KINDS = ("direct", "barnes-hut", "sycl-direct", "sycl-tree")
INTEGRATOR_KINDS = ("velocity-verlet", "yoshida4", "rk4")
INITIAL_CONDITION_KINDS = (
    "plummer",
    "uniform-sphere",
    "kepler",
    "disc-galaxy",
    "galaxy-collision",
)
EXECUTOR_KINDS = ("serial", "static", "work-stealing")

#: The Plummer scale radius that puts a unit-mass sphere into standard N-body
#: units, which is what a scale radius of zero resolves to.
#: `initial_conditions/plummer.hpp` is where the value comes from.
STANDARD_PLUMMER_RADIUS = 3.0 * 3.141592653589793 / 16.0


class ConfigurationError(Exception):
    """A file that is not a configuration, reported by line as the C++ reports it."""

    def __init__(self, line: int, message: str) -> None:
        super().__init__(f"line {line}: {message}")
        self.line = line
        self.detail = message


@dataclass(frozen=True)
class RunSettings:
    timestep: float = 0.0
    steps: int = 0
    seed: int = 0


@dataclass(frozen=True)
class InitialConditionSettings:
    kind: str = "plummer"
    count: int = 0
    total_mass: float = 1.0
    scale_radius: float = 0.0
    radius: float = 1.0
    mass_fraction_cutoff: float = 0.999
    primary_mass: float = 1.0
    secondary_mass: float = 1.0
    semi_major_axis: float = 1.0
    eccentricity: float = 0.0
    bulge_fraction: float = 0.2
    scale_length: float = 1.0
    scale_height: float = 0.1
    bulge_radius: float = 0.2
    inclination: float = 0.0
    position_angle: float = 0.0
    mass_ratio: float = 0.5
    secondary_inclination: float = 1.0
    secondary_position_angle: float = 0.0
    separation: float = 20.0
    impact_parameter: float = 2.0
    approach_speed: float = 0.8


@dataclass(frozen=True)
class SolverSettings:
    kind: str = "barnes-hut"
    softening: float = 0.0
    opening_angle: float = 0.5
    leaf_capacity: int = 32
    quadrupole: bool = False
    executor: str = "work-stealing"
    threads: int = 0
    allow_cpu_fallback: bool = True


@dataclass(frozen=True)
class IntegratorSettings:
    kind: str = "velocity-verlet"


@dataclass(frozen=True)
class OutputSettings:
    trajectory_path: str = ""
    trajectory_stride: int = 0
    trajectory_velocities: bool = False
    diagnostics_path: str = ""
    diagnostics_stride: int = 0
    checkpoint_path: str = ""
    checkpoint_stride: int = 0


@dataclass(frozen=True)
class Configuration:
    """A whole run, as data. The mirror of `sim/configuration.hpp`."""

    run: RunSettings = field(default_factory=RunSettings)
    initial_conditions: InitialConditionSettings = field(
        default_factory=InitialConditionSettings
    )
    solver: SolverSettings = field(default_factory=SolverSettings)
    integrator: IntegratorSettings = field(default_factory=IntegratorSettings)
    output: OutputSettings = field(default_factory=OutputSettings)


#: How a value is read, per section and key.
#:
#: The one place the settings table appears in this language. Everything else
#: works from it, so a setting added to the format is added here and the reader,
#: the writer and the "unknown setting" message all learn about it at once.
_TYPES: dict[str, dict[str, str]] = {
    "run": {"timestep": "number", "steps": "whole", "seed": "whole"},
    "initial_conditions": {
        "kind": "initial_condition_kind",
        "count": "whole",
        "total_mass": "number",
        "scale_radius": "number",
        "radius": "number",
        "mass_fraction_cutoff": "number",
        "primary_mass": "number",
        "secondary_mass": "number",
        "semi_major_axis": "number",
        "eccentricity": "number",
        "bulge_fraction": "number",
        "scale_length": "number",
        "scale_height": "number",
        "bulge_radius": "number",
        "inclination": "number",
        "position_angle": "number",
        "mass_ratio": "number",
        "secondary_inclination": "number",
        "secondary_position_angle": "number",
        "separation": "number",
        "impact_parameter": "number",
        "approach_speed": "number",
    },
    "solver": {
        "kind": "solver_kind",
        "softening": "number",
        "opening_angle": "number",
        "leaf_capacity": "whole",
        "quadrupole": "boolean",
        "executor": "executor_kind",
        "threads": "whole",
        "allow_cpu_fallback": "boolean",
    },
    "integrator": {"kind": "integrator_kind"},
    "output": {
        "trajectory_path": "text",
        "trajectory_stride": "whole",
        "trajectory_velocities": "boolean",
        "diagnostics_path": "text",
        "diagnostics_stride": "whole",
        "checkpoint_path": "text",
        "checkpoint_stride": "whole",
    },
}

_ENUMERATIONS: dict[str, tuple[str, ...]] = {
    "solver_kind": SOLVER_KINDS,
    "integrator_kind": INTEGRATOR_KINDS,
    "initial_condition_kind": INITIAL_CONDITION_KINDS,
    "executor_kind": EXECUTOR_KINDS,
}

_SECTION = re.compile(r"^\[([A-Za-z0-9_]+)\]$")

#: A decimal number in the classic locale, and nothing else.
#:
#: Written out rather than left to `float`, which accepts several things the C++
#: does not: underscores as digit separators, and surrounding whitespace that
#: has already been removed by the time this sees the value. The infinities and
#: the NaN are here because `std::istringstream` does read them, and because
#: `problems_with` is written to catch them: every one of its checks is the
#: negation of the condition it wants, so that a NaN fails rather than passing.
_NUMBER = re.compile(
    r"^[+-]?(?:(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?|inf|infinity|nan)$",
    re.IGNORECASE,
)

#: A whole number. `core::Index` is `std::size_t`, so a minus sign is not a
#: negative value but a parse failure, which is what `std::from_chars` does for
#: an unsigned type and what the format documentation says.
_WHOLE = re.compile(r"^\d+$")


def _read_number(key: str, value: str) -> float:
    if not _NUMBER.match(value):
        raise ConfigurationError(0, f"{key}: expected a number, found '{value}'")
    return float(value)


def _read_whole(key: str, value: str) -> int:
    if not _WHOLE.match(value):
        raise ConfigurationError(0, f"{key}: expected a whole number, found '{value}'")
    return int(value)


def _read_boolean(key: str, value: str) -> bool:
    if value not in ("true", "false"):
        raise ConfigurationError(0, f"{key}: expected true or false, found '{value}'")
    return value == "true"


def _read_value(kind: str, key: str, value: str) -> object:
    if kind == "number":
        return _read_number(key, value)
    if kind == "whole":
        return _read_whole(key, value)
    if kind == "boolean":
        return _read_boolean(key, value)
    if kind == "text":
        return value
    names = _ENUMERATIONS[kind]
    if value not in names:
        raise ConfigurationError(
            0, f"{key}: '{value}' is not one of {', '.join(names)}"
        )
    return value


def read_configuration(text: str) -> Configuration:
    """The configuration `text` describes.

    Strict in the ways `docs/formats/configuration.md` says the reader is
    strict: an unknown section or setting, a value that does not parse, a
    setting with no value, the same setting twice in either spelling, and a
    setting before any heading that does not name its own section are all
    errors. Nothing is skipped and nothing is guessed.

    :raises ConfigurationError: on the first line that is not a configuration.
    """
    values: dict[str, dict[str, object]] = {section: {} for section in _TYPES}
    section: str | None = None

    for index, raw in enumerate(text.split("\n")):
        number = index + 1
        line = raw.strip(" \t\r")
        if line == "" or line.startswith("#"):
            continue

        heading = _SECTION.match(line)
        if heading is not None:
            section = heading.group(1)
            if section not in _TYPES:
                raise ConfigurationError(
                    number,
                    f"[{section}] is not a section of this format. "
                    f"The sections are {', '.join(_TYPES)}.",
                )
            continue

        equals = line.find("=")
        if equals < 0:
            raise ConfigurationError(number, f"expected a setting, read '{line}'")

        name = line[:equals].strip()
        value = line[equals + 1 :].strip()
        if name == "":
            raise ConfigurationError(number, "a setting with no name")
        if value == "":
            raise ConfigurationError(number, f"{name} has no value")

        # A key may name its own section, and doing so does not change the
        # section the following lines belong to. That form is what `--set` uses.
        dot = name.find(".")
        where = section if dot < 0 else name[:dot]
        key = name if dot < 0 else name[dot + 1 :]
        if where is None:
            raise ConfigurationError(number, f"{name} is outside any section")
        if where not in _TYPES:
            raise ConfigurationError(number, f"{where} is not a section of this format")
        if key not in _TYPES[where]:
            raise ConfigurationError(
                number, f"{where}.{key} is not a setting of this format"
            )
        if key in values[where]:
            raise ConfigurationError(number, f"{where}.{key} is set twice")

        try:
            values[where][key] = _read_value(
                _TYPES[where][key], f"{where}.{key}", value
            )
        except ConfigurationError as error:
            raise ConfigurationError(number, error.detail) from error

    return Configuration(
        run=replace(RunSettings(), **values["run"]),
        initial_conditions=replace(
            InitialConditionSettings(), **values["initial_conditions"]
        ),
        solver=replace(SolverSettings(), **values["solver"]),
        integrator=replace(IntegratorSettings(), **values["integrator"]),
        output=replace(OutputSettings(), **values["output"]),
    )


def sections_present(text: str) -> set[str]:
    """Which sections a file sets anything in, in either spelling.

    A heading with nothing under it is not one of them, since what is being
    asked is what the document decides rather than what it names.

    Needed because a configuration is a complete structure once it has been
    read: nothing in it distinguishes a setting stated as its default from one
    left out. The service refuses a submission that decides where a run writes,
    and that refusal is about what the document says rather than about what it
    resolves to.
    """
    present: set[str] = set()
    section: str | None = None
    for raw in text.split("\n"):
        line = raw.strip(" \t\r")
        if line == "" or line.startswith("#"):
            continue
        heading = _SECTION.match(line)
        if heading is not None:
            section = heading.group(1)
            continue
        equals = line.find("=")
        if equals < 0:
            continue
        name = line[:equals].strip()
        dot = name.find(".")
        where = name[:dot] if dot >= 0 else section
        if where is not None:
            present.add(where)
    return present


def _number(value: float) -> str:
    """A number written so that reading it back gives the same value.

    `repr` for a float in Python is the shortest string that round-trips, which
    is the property wanted, but it writes whole values as `20.0` where the
    examples write `20`. Both parse to the same number and the shorter one is
    what a person would have typed.
    """
    if value == int(value) and abs(value) < 1e16:
        return str(int(value))
    return repr(value)


def write_configuration(configuration: Configuration) -> str:
    """The file that `read_configuration` would read back as `configuration`.

    Every setting is written, not only those that differ from their defaults.
    This is what the worker hands the binary, and a file that states everything
    is a file whose meaning does not depend on the defaults of the build reading
    it. `orrery show` prints the same way and for the same reason.

    The one exception is a path that is not set, which is left out rather than
    written empty. A key with nothing after the equals sign is not a legal line
    in this format, so writing one would produce a file this reader refuses.
    """
    lines: list[str] = []
    for section, keys in _TYPES.items():
        values = getattr(configuration, section)
        lines.append(f"[{section}]")
        for key, kind in keys.items():
            value = getattr(values, key)
            if kind == "text" and value == "":
                continue
            if kind == "boolean":
                written = "true" if value else "false"
            elif kind == "number":
                written = _number(value)
            else:
                written = str(value)
            lines.append(f"{key} = {written}")
        lines.append("")
    return "\n".join(lines)


def resolved_scale_radius(settings: InitialConditionSettings) -> float:
    """The scale radius the Plummer sampler will actually use.

    Zero means the standard N-body value, as it does in `configuration.cpp`.
    """
    return (
        settings.scale_radius if settings.scale_radius > 0 else STANDARD_PLUMMER_RADIUS
    )
