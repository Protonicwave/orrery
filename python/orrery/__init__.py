"""Orrery: a GPU-accelerated N-body gravitational simulator.

The compiled module is ``orrery._orrery`` and everything it defines is re-exported
here. This file adds the two things that are better said in Python than bound
from C++: the NumPy scalar type the library was built for, and the one helper
that turns the component arrays into the shape a plotting library expects.

A first run::

    import orrery

    configuration = orrery.Configuration()
    configuration.initial_conditions.kind = orrery.InitialConditionKind.plummer
    configuration.initial_conditions.count = 4096
    configuration.run.timestep = 1.0 / 64.0
    configuration.run.steps = 512
    configuration.solver.softening = 0.01

    simulation = orrery.assemble(configuration)
    before = simulation.measure()
    simulation.run(configuration.run.steps)
    after = simulation.measure()

    print((after.total_energy - before.total_energy) / abs(before.total_energy))

The state is reached without copying::

    x = simulation.particles.position_x   # a NumPy view, not a copy

which is the point of the bindings rather than a detail of them: at the sizes
this project runs at, a copy per frame costs more than the physics.
"""

from __future__ import annotations

import numpy as _numpy

from . import _orrery as _extension
from ._orrery import *  # noqa: F403
from ._orrery import __version__, single_precision

#: The NumPy scalar type of every array this library hands out.
#:
#: It follows the precision the library was built with rather than being assumed,
#: because a single-precision build and a double-precision build of the same
#: source disagree about the size of every scalar in every interface. A test or a
#: notebook that needs a tolerance should take it from here rather than writing
#: one that silently asserts nothing in the other build.
dtype = _numpy.dtype(_numpy.float32 if single_precision else _numpy.float64)

#: The three components of each vector quantity, in the order they are stored.
_COMPONENTS = ("x", "y", "z")

#: The vector quantities a particle store holds.
_QUANTITIES = ("position", "velocity", "acceleration")


def components(particles, quantity: str = "position") -> tuple:
    """The three component views of a vector quantity, as a tuple.

    No copy is made: each element is a NumPy view of the array inside
    ``particles``, and the tuple exists only to carry the three together.

    >>> x, y, z = orrery.components(simulation.particles)
    """
    if quantity not in _QUANTITIES:
        raise ValueError(
            f"{quantity!r} is not a vector quantity of a particle store; "
            f"expected one of {', '.join(_QUANTITIES)}"
        )
    return tuple(getattr(particles, f"{quantity}_{axis}") for axis in _COMPONENTS)


def stacked(particles, quantity: str = "position"):
    """A copy of a vector quantity as an ``(N, 3)`` array.

    **This copies.** The storage is three separate contiguous arrays, one per
    component (ADR-0004), so an ``(N, 3)`` array of positions exists nowhere in
    memory and cannot be viewed into being. The copy is here because a plotting
    library wants that shape, and it is a named function rather than a property
    so that the cost appears at the call site.

    Nothing in a simulation needs this. Use `components` to read or write the
    state, and this only to hand it to something that insists on triples.
    """
    return _numpy.column_stack(components(particles, quantity))


def radii(particles):
    """The distance of each particle from the origin.

    A copy, of one array rather than three, since it is a reduction over the
    components rather than a rearrangement of them. It is here because almost
    every plot of a sampled model begins with it.
    """
    x, y, z = components(particles)
    return _numpy.sqrt(x * x + y * y + z * z)


__all__ = [
    name for name in dir(_extension) if not name.startswith("_")
] + [
    "__version__",
    "components",
    "dtype",
    "radii",
    "single_precision",
    "stacked",
]
