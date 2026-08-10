# ADR-0023: Open a cell on the distance to its centre of mass, corrected for where that is

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

The opening criterion is the whole of Barnes-Hut. It decides, for each cell and
each particle, whether the cell may stand in for its contents, and everything
the method claims about accuracy is a claim about that decision.

The criterion in the original paper is

    s / d < theta

where s is the cell's side, d is the distance from the particle to the cell's
centre of mass, and theta is a parameter. It is one comparison and it is what
most descriptions of the method state.

It has a known failure, first set out by Salmon and Warren, and the failure is
not a rare one. The expansion being truncated is about the centre of mass, and
it converges for field points outside a sphere centred there and containing all
of the cell's mass. The criterion measures d from the centre of mass but takes s
from the cell's geometry, and the two are only interchangeable when the mass is
distributed evenly in the cell. When it is not, when a cell of a clustered
configuration holds all of its mass in one corner, the centre of mass sits near
that corner, and a particle just outside the opposite face of the cell can be
much closer to some of the cell's particles than d suggests. The test passes and
the expansion is evaluated outside its radius of convergence.

A Plummer sphere is exactly the configuration that produces these cells, in the
transition between the core and the halo, and it is the configuration this
project validates on.

## Decision

A cell is accepted when

    d >= s / theta + delta

where delta is the distance from the cell's geometric centre to its centre of
mass. The opening angle is restricted to `[0, 1]`.

The right-hand side is precomputed per node during construction, and squared, so
that the traversal compares one squared distance against one stored number.

## Alternatives considered

**The classical criterion.** One fewer subtraction per node during construction
and no per-node storage, in exchange for an error bound that does not hold on the
configurations this project runs. The correction is paid once per node at build
time and never during a walk, so it costs nothing where the cost would matter.

**A criterion on the multipole acceptance error.** Estimate the size of the
first neglected term for each cell and accept when it is below an absolute
tolerance. This is the better method, and it is what a code aiming at a stated
error rather than a stated angle should do: it spends effort where the error is
rather than where the geometry is, and it produces a bound on the answer instead
of a bound on the opening angle. It is not adopted here because it needs a
tolerance in units of acceleration, which is a property of the configuration
rather than of the algorithm, and because comparing against published Barnes-Hut
results means using the parameter those results are quoted in terms of. The
error against cost curve in `docs/performance/barnes_hut.md` is the measurement
that would justify moving to one, and it is what a later phase should start from.

**Leaving the opening angle unrestricted.** Above one, the criterion can accept
a cell that contains the particle being accelerated: the particle is then
accelerated by a centre of mass its own mass contributed to, which is a particle
attracting itself. Unsoftened it is worse than inaccurate, because the
separation can be zero and the result a NaN that propagates into every later
step. The bound is therefore a correctness condition rather than a quality
setting, and the reason it can be stated so simply is the criterion above: with
delta included, a particle inside a cell is at most `s * sqrt(3) / 2 + delta`
from the centre of mass and the criterion demands at least `s / theta + delta`,
so any theta at or below one opens the cell.

A request for a larger angle is reduced to one rather than rejected, on the
precedent of `DirectSolver::select_kernel`: the solver produces correct physics
and reports what it settled on, and a caller that cares asks.

## Consequences

Every node carries one extra floating-point number, the squared acceptance
radius, and construction pays one division and one square root per node to
compute it. Neither is measurable against the traversal.

The opening angle is not the classical one and results are not directly
comparable with codes that use the classical form: for the same theta this
criterion opens more cells, so it is more accurate and slower. The difference is
largest exactly where it matters, on cells whose mass is off centre. Anyone
comparing against published figures should compare error against cost rather
than error against theta, which is why `docs/performance/barnes_hut.md` reports
the two together and why the interaction counter reports cells and pairs
separately.

The criterion is a property of the tree rather than of the walk, since the
radius is baked into each node when the tree is built. A tree is therefore built
for one opening angle and cannot be reused at another. That is not a restriction
in practice, since the tree is rebuilt every evaluation anyway (ADR-0022), and
it is what lets the traversal decide in a single comparison.
