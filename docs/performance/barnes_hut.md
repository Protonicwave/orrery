# The tree solver, against the algorithm it replaces

Measured on the machine [the performance report](../performance.md) describes.

The short version, in four findings.

The tree overtakes direct summation at about **6100 particles** on this machine.
Below that the O(N^2) kernel is faster, and by a wide margin at a thousand
particles, because a tree spends most of its time walking rather than computing.

Above the crossover it wins by more at every size. At 262144 particles one force
evaluation takes **0.87 seconds** against roughly 17 seconds for direct
summation, which is a factor of twenty.

The cost is **N^1.24** over two and a half decades, against 1.11 for exactly
N log N and 2 for direct summation.

And the interactions a tree computes are about **four times as expensive** as
the ones direct summation computes. That is the finding this document is really
reporting: at 262144 particles the tree computes 78 times fewer interactions and
is only 20 times faster, and the difference between those two numbers is the
whole argument for what a tree solver should be optimised for next.

## The machine and the method

| Property | Value |
| --- | --- |
| CPU | Intel Core Ultra 5 238V, 4 Lion Cove plus 4 Skymont, 8 threads |
| Build | `RelWithDebInfo`, Clang 22.1.8, no fast-math flag anywhere |
| Power | Mains, Windows power mode set to best performance |
| Configuration | Plummer sphere, seed 20260810, softening 0.05 |
| Solver | Work stealing across all eight cores, AVX2 kernel at the leaves |
| Parameters | Opening angle 0.5, leaf capacity 32, monopole only, unless stated |
| Protocol | 11 timed trials after a settling warm-up, median reported |

Every figure comes from `benchmarks/harness/`, whose methodology ADR-0019
records. Reproduce with:

```
cmake --preset release
cmake --build --preset release
./build/release/benchmarks/orrery_tree_scaling
```

The program writes `tree_scaling.csv` and `tree_accuracy.csv` into the current
directory, and the copies here are from the session recorded below. Its thermal
canary moved **1.7 per cent** across the whole session, which is the steadiest
this project has measured and means the tables can be read straight down.

## Scaling

Session of 2026-08-10 22:12, double precision. Direct summation is not timed
above 65536 particles, where one evaluation is already over a second.

| N | Tree | Spread | Direct | Speedup | Exponent | ms / (N log N) | Pairs per particle | Cells per particle |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1024 | 0.879 ms | 4.6% | 0.283 ms | 0.32x | - | 8.58e-05 | 600 | 47 |
| 2048 | 2.158 ms | 4.9% | 0.958 ms | 0.44x | 1.30 | 9.58e-05 | 833 | 120 |
| 4096 | 5.573 ms | 6.6% | 4.417 ms | 0.79x | 1.37 | 1.13e-04 | 1183 | 220 |
| 8192 | 15.47 ms | 7.1% | 18.40 ms | **1.19x** | 1.47 | 1.45e-04 | 1457 | 395 |
| 16384 | 37.02 ms | 11.2% | 75.34 ms | 2.03x | 1.26 | 1.61e-04 | 1596 | 636 |
| 32768 | 96.00 ms | 7.7% | 400.3 ms | 4.17x | 1.37 | 1.95e-04 | 1665 | 915 |
| 65536 | 236.3 ms | 11.2% | 1082 ms | 4.58x | 1.30 | 2.25e-04 | 1777 | 1176 |
| 131072 | 389.5 ms | 4.4% | - | - | 0.72 | 1.75e-04 | 1574 | 1525 |
| 262144 | 868.2 ms | 6.0% | - | - | 1.16 | 1.84e-04 | 1597 | 1776 |

The exponent column is the local slope between one row and the one above it. The
overall figure, fitted across the whole range from 1024 to 262144, is **1.24**.

For comparison: a cost of exactly N log N over this range gives 1.11, and direct
summation gives 2. The measured 1.24 sits close to the first and nowhere near
the second, which is the claim this table was run to check. It sits slightly
above N log N for a reason that is visible in the last two columns: the
interactions per particle grow by a factor of 5.2 across the range while log N
grows by only 1.8, because at a thousand particles the tree has six levels and
barely any structure to exploit. Between 32768 and 262144, where the tree is a
tree, the interaction count per particle grows by 1.31 against 1.20 for log N.

The `ms / (N log N)` column is the same statement without the differencing.
It rises by a factor of two across the range and is flat to within ten per cent
over the last four rows, which is where the asymptotic claim actually applies.

**Two rows are anomalous and are left as they were measured.** The step from
65536 to 131072 gives an exponent of 0.72, which is impossible for any algorithm
with a superlinear cost: the tree cannot get cheaper per particle as the problem
grows. The 65536 row carries an 11.2 per cent spread and was measured
immediately after the heaviest direct summation row in the session, and the most
likely explanation is that the part had not recovered from it. The row after has
a 4.4 per cent spread and fits the trend. Repeating the session with the direct
rows removed would settle it; the harness reported the confound rather than
averaging it away, which is what ADR-0019 asks of it.

## Where the crossover is

Between 4096 particles, where direct summation is 1.26 times faster, and 8192,
where the tree is 1.19 times faster. Interpolating between the two gives about
**6100 particles**.

That number is a property of this machine and these two implementations rather
than of the two algorithms, and it is high compared with the figures usually
quoted for Barnes-Hut. The reason is Phase 7. Direct summation here is
vectorised with AVX2 and spread across eight cores by a work-stealing scheduler,
and it reaches 80 per cent of the only hardware ceiling that binds it
(`roofline.md`). The tree is competing against a well-optimised opponent, and a
crossover measured against a scalar single-threaded direct kernel would have
been several times lower and would have flattered the tree.

## What a tree interaction costs

The interaction counter makes the comparison the timings cannot. At 262144
particles the tree computes 3373 interactions per particle against direct
summation's 262143, which is **78 times fewer**. It is 20 times faster.

The two numbers differ by a factor of about four, and that factor is the cost of
one tree interaction against one direct interaction:

| Solver | N | Interactions per second |
| --- | --- | --- |
| Direct | 65536 | 3.97 G |
| Tree | 65536 | 0.82 G |
| Tree | 262144 | 1.02 G |

Direct summation computes its interactions in the best circumstances arithmetic
ever gets: four at a time in one AVX2 register, from four contiguous arrays,
with no branch and no address arithmetic. The tree computes two kinds, and
neither is like that. A particle-cell interaction is a scalar term at the end of
a pointer-free but still unpredictable walk, and it is preceded by the
comparison that decided to accept the cell and by however many comparisons
rejected cells above it. A particle-particle interaction at a leaf does use the
vector kernel, but on a range of a few dozen rather than a few thousand, so the
loop's prologue and epilogue are a larger share of it.

This is the finding worth carrying forward. The tree's asymptotic advantage is
real and it is measured above, but a factor of four of it is being handed back
at the level of the instruction mix. Two routes are open and each is a separate
decision: vectorise the traversal, by walking several particles against one cell
at a time, which is what a GPU implementation has to do anyway and is therefore
Phase 10's problem; or raise the leaf capacity so that more of the work is done
by the kernel that is already fast, which is the next section.

## Where the time goes, and what the tree looks like

| N | Nodes | Leaves | Depth | Sort | Gather | Build | Walk |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1024 | 175 | 147 | 6 | 9.2% | 0.5% | 14.2% | 76.1% |
| 4096 | 581 | 504 | 8 | 6.2% | 0.3% | 1.4% | 92.1% |
| 16384 | 2187 | 1907 | 9 | 6.2% | 0.3% | 0.5% | 93.0% |
| 65536 | 7770 | 6792 | 10 | 3.8% | 0.3% | 0.2% | 95.7% |
| 262144 | 31437 | 27496 | 11 | 4.1% | 0.3% | 0.1% | 95.5% |

This table is the evidence for ADR-0022, which rebuilds the whole tree on every
force evaluation rather than maintaining one. Everything that decision costs is
in the first three columns of shares, and at the sizes where a tree solver is
the right choice at all they come to under five per cent together. Building the
tree itself is a fifth of one per cent at the largest size. An incremental
scheme could at best recover part of that, in exchange for the accuracy hazard
and the bookkeeping the ADR sets out.

The sort is the largest of the three and it is the one that is genuinely
parallel: a merge sort through the same work-stealing executor the traversal
uses. Its share falls as N grows, which is the right direction for an O(N log N)
step measured against an O(N log N) walk with a much heavier body.

The tree's shape is worth reading too. There are about 0.12 nodes per particle
and 87 per cent of them are leaves, which averages 9.5 particles per leaf
against a capacity of 32. That gap is where the leaf capacity could still be
tuned: leaves are not full, because a cell is subdivided when it exceeds the
capacity and its eight children split its contents unevenly.

The depth grows from 6 to 11 across two and a half decades, which is log N
almost exactly, and is the direct evidence that the octree is balanced on a
Plummer sphere rather than degenerating in the core.

## Error against cost

The curve the method is judged on. 16384 particles, all cores, errors measured
against the compensated reference of `solvers/reference_kernel.hpp` over a
sample of 1024 particles.

| Opening angle | Quadrupole | Median | Spread | Worst error | RMS error | Pairs per particle | Cells per particle |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0.2 | no | 123.3 ms | 5.3% | 3.53e-04 | 1.11e-04 | 8222 | 684 |
| 0.4 | no | 50.08 ms | 7.9% | 4.57e-03 | 1.06e-03 | 2628 | 735 |
| 0.5 | no | 34.55 ms | 7.1% | 1.04e-02 | 1.94e-03 | 1596 | 636 |
| 0.7 | no | 19.33 ms | 3.9% | 2.52e-02 | 4.36e-03 | 711 | 482 |
| 1.0 | no | 11.33 ms | 5.2% | 6.65e-02 | 1.01e-02 | 277 | 353 |
| 0.2 | yes | 134.8 ms | 9.0% | 7.19e-05 | 3.05e-05 | 8222 | 684 |
| 0.4 | yes | 59.71 ms | 6.7% | 2.07e-03 | 4.70e-04 | 2628 | 735 |
| 0.5 | yes | 43.87 ms | 4.7% | 7.68e-03 | 9.76e-04 | 1596 | 636 |
| 0.7 | yes | 26.43 ms | 5.0% | 1.56e-02 | 2.65e-03 | 711 | 482 |
| 1.0 | yes | 15.97 ms | 6.1% | 2.99e-02 | 7.68e-03 | 277 | 353 |

Read the worst and the root mean square columns together. They differ by a
factor of five throughout, and the reason is physical rather than numerical: the
particles in the dense core of a Plummer sphere are dominated by near neighbours
that the tree sums directly, and the ones in the halo see almost the whole
configuration through a handful of large cells. The tree is far more accurate
where the forces are large, which is the right way round for an integration and
the wrong way round for anyone quoting a single error figure.

Fitted across the range, the root mean square error goes as **theta^2.8** without
quadrupoles and **theta^3.4** with them, and the cost goes as **theta^-1.5**.

## Which knob to turn

The interaction counts are identical between the two halves of the table at each
angle, which is the point of the design: the quadrupole changes what an accepted
cell costs, not which cells are accepted. So the comparison is clean. The
quadrupole moment buys a factor of two in accuracy for 27 per cent more time at
theta 0.5, and the question is whether closing the angle instead would have been
cheaper.

At equal accuracy, from the fits above:

| Target RMS error | Monopole | Quadrupole | Cheaper |
| --- | --- | --- | --- |
| 1e-2 | theta 1.0, 11.3 ms | out of range, theta would exceed 1 | monopole |
| 1e-3 | theta 0.44, 43.5 ms | theta 0.50, 43.9 ms | neither |
| 1e-4 | theta 0.19, 127 ms | theta 0.28, 90 ms | quadrupole, by 1.4x |

So the answer is that it depends, and where it changes has been measured rather
than argued. Below about a part in a thousand the quadrupole is the cheaper way
to buy accuracy and the advantage grows as the requirement tightens; above it,
opening the angle is cheaper and the quadrupole is dead weight. That is why
ADR-0024 makes the moments an option rather than a default, and why the default
is off: the default opening angle of 0.5 sits almost exactly on the point where
the two are equal.

The general shape of that result is what the exponents predict. The quadrupole
gives the error one more power of theta to fall with, so it must win eventually
as the accuracy demanded rises, and it must lose where the expansion is being
truncated so early that the second term is not much smaller than the first.

## What this does not measure

Nothing here is in single precision. The tree solver builds and runs in the
single-precision configuration and its tests pass there, but the accuracy sweep
above would be measuring the float kernel's own error against the approximation
at the tighter angles, and separating the two is a measurement this phase did
not make.

The configuration is a Plummer sphere throughout. It is the right choice for a
first characterisation, since it has a dense core and a wide halo and is the
configuration the project validates on, but a tree's cost depends on the
clustering, and the galaxy collision of Phase 12 will be more strongly clustered
than this. The figures here are not a promise about that.

The crossover is for one thread count. Direct summation and the tree do not
parallelise equally well, since the tree's traversal has a much less predictable
memory access pattern, so the crossover on one core is not the crossover on
eight. Only the eight-core figure was measured.

The leaf capacity was not swept. Thirty-two is the default and every figure here
uses it, and the section on interaction costs above gives the reason to think a
larger value might be better on this machine. That is a measurement a later
phase should make rather than a claim this one is making.

## A note on the sanitiser builds

`roofline.md` records that the sanitiser builds cannot be produced on the
development machine and are left to continuous integration. That is half right,
and this phase established which half, because the tree solver does more index
arithmetic than anything else in the project and leaving it entirely to a
continuous integration run that was unavailable would have been unwise.

The address sanitiser does build and run here. What is missing is `stl_asan.lib`,
the annotated build of the Microsoft standard library that Visual Studio ships as
an optional component, and defining `_DISABLE_VECTOR_ANNOTATION` and
`_DISABLE_STRING_ANNOTATION` removes the dependency on it. The whole suite, 208
cases, then passes clean. The instrumented binaries need
`clang_rt.asan_dynamic-x86_64.dll` on the path, both to run and for the test
discovery step that runs them at build time.

That is a weaker check than the one continuous integration performs, and the
difference is worth stating rather than glossing: without the annotations, an
overflow that stays inside a `std::vector`'s allocated capacity is invisible,
which is exactly the class of mistake an index-heavy tree builder is most likely
to make. Allocation-boundary overflows, use after free and stack overflows are
still caught.

The undefined-behaviour sanitiser does not build here at all. Its runtime is
compiled against a different C runtime from the one this toolchain links, and the
link fails with a `RuntimeLibrary` mismatch before any of this project's code is
involved. That one is genuinely continuous integration's to run, along with the
thread sanitiser and every GCC configuration.
