"""The Python reader and the C++ reader, put through the same files.

This is what makes `orrery_service/configuration.py` a mirror rather than a
second specification. Every case below goes to both readers, and they have to
agree about whether the file is a run and, where they both got as far as
looking, about which settings are wrong with it.

`orrery show` is the C++ half: it reads a configuration, prints what the
settings resolve to, reports every objection on its error stream and exits
non-zero if there were any. It takes no steps, so this suite costs the time of
starting a process rather than the time of a simulation.

The test skips itself, with the reason, when there is no binary to compare
against. That is the common case on a machine where somebody is working on the
service alone, and continuous integration builds the release preset before
running this, which is where the comparison actually gets made.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from orrery_service.configuration import ConfigurationError, read_configuration
from orrery_service.validation import problems_with

REPOSITORY = Path(__file__).resolve().parents[2]

#: How the C++ reports one objection: `orrery: setting: complaint`.
_PROBLEM = re.compile(r"^orrery: ([a-z_]+\.[a-z_]+): ", re.MULTILINE)


def _binary() -> str | None:
    """The simulator to compare against, or None if this machine has none.

    ORRERY_BINARY first, so that continuous integration and a container can say
    exactly which build is being compared. Otherwise the build trees the presets
    write to, newest first, because somebody working on this locally has usually
    just built one.
    """
    named = os.environ.get("ORRERY_BINARY", "").strip()
    if named:
        return named if Path(named).exists() or shutil.which(named) else None

    found = [
        path
        for pattern in ("build/*/apps/orrery", "build/*/apps/orrery.exe")
        for path in REPOSITORY.glob(pattern)
    ]
    if not found:
        return None
    return str(max(found, key=lambda path: path.stat().st_mtime))


BINARY = _binary()

pytestmark = pytest.mark.skipif(
    BINARY is None,
    reason=(
        "no orrery binary to compare against. Build one with "
        "`cmake --build --preset release`, or name it in ORRERY_BINARY."
    ),
)


def _show(text: str, tmp_path: Path) -> tuple[bool, set[str]]:
    """What the C++ makes of `text`: whether it would run, and what it objected to."""
    path = tmp_path / "case.orrery"
    path.write_text(text, encoding="utf-8", newline="\n")
    result = subprocess.run(
        [str(BINARY), "show", str(path)],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    return result.returncode == 0, set(_PROBLEM.findall(result.stderr))


def _here(text: str) -> tuple[bool, set[str]]:
    """And what this reader makes of it."""
    try:
        configuration = read_configuration(text)
    except ConfigurationError:
        return False, set()
    problems = problems_with(configuration)
    return not problems, {problem.setting for problem in problems}


#: Configurations that should be accepted, one per scenario the format has.
ACCEPTED = [
    "[run]\ntimestep = 0.001\nsteps = 100\n[initial_conditions]\ncount = 64\n",
    "[run]\ntimestep = 0.001\nsteps = 100\n[initial_conditions]\n"
    "kind = uniform-sphere\ncount = 64\nradius = 2\n",
    "[run]\ntimestep = 0.001\nsteps = 100\n[initial_conditions]\n"
    "kind = kepler\neccentricity = 0.6\n",
    "[run]\ntimestep = 0.001\nsteps = 100\n[initial_conditions]\n"
    "kind = disc-galaxy\ncount = 512\ninclination = 0.4\n",
    "[run]\ntimestep = 0.001\nsteps = 100\n[initial_conditions]\n"
    "kind = galaxy-collision\ncount = 512\nmass_ratio = 0.25\n",
    "[run]\ntimestep = 0.001\nsteps = 100\n[initial_conditions]\ncount = 64\n"
    "[solver]\nkind = direct\nsoftening = 0.05\n",
    "[run]\ntimestep = 0.001\nsteps = 100\n[initial_conditions]\ncount = 64\n"
    "[solver]\nkind = sycl-tree\nopening_angle = 0.7\nquadrupole = true\n"
    "[integrator]\nkind = yoshida4\n",
    "[run]\ntimestep = 0.001\nsteps = 100\n[initial_conditions]\ncount = 64\n"
    "[output]\ntrajectory_path = out.otj\ntrajectory_stride = 10\n"
    "diagnostics_path = out.csv\ndiagnostics_stride = 10\n",
]

#: Configurations that should be refused, covering every check in
#: `problems_with` and a spread of the reader's own strictness.
REFUSED = [
    # The reader's strictness.
    "[nonsense]\nx = 1\n",
    "[run]\nsoftenning = 1\n",
    "[run]\ntimestep = 0.5 and a bit\n",
    "[run]\ntimestep = 1.0e\n",
    "[run]\nsteps = -1\n",
    "[solver]\nquadrupole = yes\n",
    "[solver]\nkind = octree\n",
    "[run]\ntimestep =\n",
    "[run]\ntimestep = 1\ntimestep = 2\n",
    "[run]\ntimestep = 1\nrun.timestep = 2\n",
    "timestep = 1\n",
    # The validator's rules.
    "[run]\nsteps = 10\n",
    "[run]\ntimestep = -1\nsteps = 10\n",
    "[run]\ntimestep = 1\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\ncount=1\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\ncount=4\ntotal_mass=0\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\ncount=4\nmass_fraction_cutoff=1\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\ncount=4\nscale_radius=-1\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=uniform-sphere\n"
    "count=4\nradius=0\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=kepler\neccentricity=1\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=kepler\nprimary_mass=0\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=kepler\ncount=2\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=galaxy-collision\ncount=2\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=galaxy-collision\n"
    "count=8\nmass_ratio=2\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=galaxy-collision\n"
    "count=8\nseparation=0\nimpact_parameter=0\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=disc-galaxy\n"
    "count=8\nscale_height=0\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\nkind=disc-galaxy\n"
    "count=8\nbulge_fraction=1\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\ncount=4\n[solver]\nsoftening=-1\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\ncount=4\n"
    "[solver]\nopening_angle=2\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\ncount=4\n"
    "[solver]\nleaf_capacity=0\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\ncount=4\n"
    "[solver]\nkind=direct\nopening_angle=0.7\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\ncount=4\n"
    "[solver]\nkind=direct\nquadrupole=true\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\ncount=4\n"
    "[output]\ntrajectory_stride=5\n",
    "[run]\ntimestep=1\nsteps=1\n[initial_conditions]\ncount=4\n"
    "[output]\ntrajectory_path=a.bin\ncheckpoint_path=a.bin\n",
]


@pytest.mark.parametrize("text", ACCEPTED)
def test_both_readers_accept(text: str, tmp_path: Path) -> None:
    native, _ = _show(text, tmp_path)
    here, problems = _here(text)
    assert native, "the binary refused a configuration this suite expects it to take"
    assert here, f"this reader refused it, objecting to {sorted(problems)}"


@pytest.mark.parametrize("text", REFUSED)
def test_both_readers_refuse(text: str, tmp_path: Path) -> None:
    native, _ = _show(text, tmp_path)
    here, _ = _here(text)
    assert not native, "the binary took a configuration this suite expects it to refuse"
    assert not here, "this reader took it"


@pytest.mark.parametrize("text", REFUSED)
def test_the_same_settings_are_named(text: str, tmp_path: Path) -> None:
    """Where both readers got as far as validating, they object to the same settings.

    Only where both did. A file that fails to parse stops at the line that
    failed, so neither reader has an opinion about the settings after it, and
    comparing what they named would be comparing two empty answers to a question
    that was not asked.
    """
    _, native = _show(text, tmp_path)
    _, here = _here(text)
    if not native or not here:
        pytest.skip("refused by the reader rather than by the validator")
    assert native == here
