# The performance report

Every speed the project claims, gathered into one document, each expressed as a
fraction of a limit measured on the machine it was measured on.

That rule is the whole of section 1's second goal and it is worth stating before
any number appears. A raw timing says nothing on its own: it depends on the part,
the power state, the compiler and the size of the problem. A fraction of a
measured ceiling says how much of the machine a kernel is using and, more
usefully, how much is left. Several of the conclusions below are that there is
nothing left, and that is a result rather than a disappointment.

The detailed documents behind this one, phase by phase, are
[`performance/threading.md`](performance/threading.md),
[`performance/roofline.md`](performance/roofline.md),
[`performance/barnes_hut.md`](performance/barnes_hut.md),
[`performance/sycl_direct.md`](performance/sycl_direct.md) and
[`performance/sycl_tree.md`](performance/sycl_tree.md). Each carries the full
tables, the per-worker breakdowns and an account of which of its figures
reproduce and which do not. This report gathers the conclusions and the numbers
that survive them.

## The machine

| Property | Value |
| --- | --- |
| CPU | Intel Core Ultra 5 238V, Lunar Lake |
| Cores | 4 Lion Cove performance plus 4 Skymont efficiency, 8 threads, no SMT |
| Vector width | AVX2, 256-bit. No AVX-512 on this part |
| L3 cache | 8 MB |
| Memory | 32 GB LPDDR5X-8533, on package, nominally about 135 GB/s |
| GPU | Intel Arc 130V, Xe2, 7 Xe-cores, memory unified with the host |
| Build | `RelWithDebInfo`, Clang 22.1.8 for the CPU figures, oneAPI DPC++ 2025.1.1 for the GPU |
| Power | Mains, Windows power mode set to best performance |

Every figure in this project was taken on that one laptop, and the point of
naming it here is that none of them should be read as a property of the
algorithms alone.

## The method

`benchmarks/harness/` implements the protocol and ADR-0019 records it: a settling
warm-up whose trials are discarded, a fixed number of timed trials, the **median**
reported rather than the best of them, an interquartile range beside every
median, a drift figure comparing the second half of a run against the first, a
cool-down between configurations, and a thermal canary bracketing the whole
session.

Every one of those exists because the target is a laptop. A best-of-N on a part
that throttles reports the trial taken before the fan noticed; a median with a
spread beside it reports the machine. **Read the spread column.** A row carrying a
27 per cent interquartile range does not support a conclusion that a 3 per cent
row would, and the sections below say which rows are which rather than quoting
all of them to three figures.

## The ceilings this machine actually has

Measured with the same harness, on the same machine, in the same session as the
kernels they judge.

| Probe | CPU, double | CPU, single | Arc 130V |
| --- | --- | --- | --- |
| Read bandwidth | 95.7 GB/s | 92.4 GB/s | 28.6 GB/s |
| Triad bandwidth | 75.2 GB/s | | |
| Fused multiply-add | 330 Gflop/s | 571 Gflop/s | 2781 Gflop/s |
| Divide and square root | 12.2 Gop/s | 27.4 Gop/s | 672 Gop/s |

Three of those rows decide most of what follows.

**Read bandwidth is 71 per cent of the nominal figure.** The specification says
about 135 GB/s; a read-only stream over a buffer sixty-four times the size of the
last-level cache reaches 95.7. That is an ordinary result for a real program, and
a roofline drawn against 135 would place every kernel further below the line than
it is and would blame the kernel for the memory controller.

**The divide and square root ceiling is a twenty-seventh of the multiply-add
ceiling on the CPU.** Every pairwise interaction in direct summation contains one
square root and one division, and they retire on a unit that is only partly
pipelined and that the other eighteen operations do not use. That ratio, not the
multiply-add peak, is what bounds this algorithm on this part.

**On the GPU the same ratio is 4.1 rather than 27.** The device's divide unit is
comparatively far stronger, which is why the GPU kernel is bound by something
else entirely and why the two processors have to be judged against different
ceilings.

The GPU bandwidth figure is reported as measured and should not be quoted as this
device's memory bandwidth. 28.6 GB/s on the same physical memory the CPU reaches
95.7 GB/s on is far more likely to be a limitation of the probe than of the
device. It affects no conclusion here, because the kernels that matter are three
orders of magnitude away from being bandwidth-bound, and
[`performance/sycl_direct.md`](performance/sycl_direct.md) says so at length.

## What one interaction costs

Every rate below is formed from a count of operations, so the count is stated
rather than assumed. One pairwise interaction is three subtractions, five
operations for the squared separation, one add for the softening, one square
root, one division, three multiplies and six operations to accumulate: **twenty
floating-point operations**, counting the square root and the division as one
each, which is the convention the N-body literature uses and what makes these
figures comparable with published ones.

A force evaluation reads `4N` values and performs `20 N (N - 1)` operations, so
the arithmetic intensity is `0.625 (N - 1)` flop per byte, which at 8192
particles is 5119 against a ridge point of 3.45. **Direct summation is
compute-bound by three and a half orders of magnitude.** That is the exception in
this project rather than the rule, and it is the exception for the same reason it
is expensive: it does N-squared work on N data.

## The CPU direct kernel

Double precision, 8192 particles, 21 timed trials:

| Kernel | Threads | Per evaluation | flop/s | Of the multiply-add peak | Of the divide ceiling |
| --- | --- | --- | --- | --- | --- |
| scalar | 1 | 177.6 ms | 7.56 G | 2.3% | 6.2% |
| scalar | 8 | 35.57 ms | 37.7 G | 11.4% | 30.9% |
| avx2 | 1 | 53.13 ms | 25.3 G | 7.6% | 20.7% |
| avx2 | 8 | **13.80 ms** | **97.3 G** | 29.5% | **79.7%** |

| Comparison | Double | Single |
| --- | --- | --- |
| Vectorising, one thread | 3.34x | 6.36x |
| Threading the vector kernel | 3.85x | 3.50x |
| Both together | **12.87x** | **22.28x** |

The last column of the first table is the result and the second-to-last is the
trap. At 29.5 per cent of the multiply-add peak it looks as though two thirds of
the machine is going to waste. At 79.7 per cent of the square root and division
throughput it is clear that almost nothing is: two operations in every twenty
compete for a unit the other eighteen cannot use, and the missing fifth is those
eighteen, the loads and the loop competing for issue slots.

**What that means is that further work on this kernel has to change the
arithmetic rather than the code.** Three routes exist and each is a separate
decision: approximate the reciprocal square root, which ADR-0020 declines;
move to single precision, where the same unit is twice as fast and the kernel
follows it almost exactly at 1.97 times; or stop computing most of the
interactions, which is the tree solver.

Vectorising also made the kernel **more accurate**, by 3.5 times in double
precision and 5.7 in single. That measurement is in
[the validation report](validation.md), because it is a statement about
correctness that happens to have come out of a performance phase.

## Threading eight cores that are not alike

The first place the target hardware shaped the design rather than the other way
round.

| Kernel | Performance core | Efficiency core | Ratio | Eight cores are worth |
| --- | --- | --- | --- | --- |
| Scalar | 207 ms | 448 ms | 2.17 | 5.84 performance cores |
| AVX2 | 51.5 ms | 327.0 ms | **6.34** | **4.63** performance cores |

The two kinds of core differ far more in vector throughput than in scalar, so
vectorising made the machine faster and made it less parallel, and both are real.
A speedup on this part should be read against 4.63 rather than against 8.

| Scheme | Speedup over one performance core | Fraction of the 4.63 limit | Performance cores idle |
| --- | --- | --- | --- |
| Equal fixed shares | 3.63x | 78% | 83.4% |
| Work stealing | 4.13x | 89% | 8.2% |

Under equal fixed shares the four performance cores spend five sixths of every
force evaluation waiting for the efficiency cores to finish an equal share of the
work. That is the failure mode section 2 of the implementation plan predicted
from the topology, quantified.

**The strongest result here is not the speedup.** Phase 6 measured the scheduler
on the scalar kernel and wrote down a prediction: that the balance it found would
follow the hardware ratio without changes when the kernel was vectorised. It did.
The share the performance cores took moved from 2.38 to one to **5.29 to one**,
tracking a hardware ratio that had gone from 2.17 to 6.34, and nothing in
`WorkStealingExecutor` changed between the two measurements. It holds no weights,
no calibration and no topology.

A weight tuned in Phase 6 would have been wrong by a factor of nearly three by
Phase 7, and would have been wrong silently. That is the argument for measuring
the balance continuously rather than baking one in, and it is ADR-0016.

## The tree solver

Barnes-Hut replaces distant groups of particles with one term each, which turns
the cost from N^2 into something close to N log N and introduces an error the
opening angle controls.

| N | Tree | Direct | Speedup | Interactions per particle |
| --- | --- | --- | --- | --- |
| 1024 | 0.879 ms | 0.283 ms | 0.32x | 647 |
| 4096 | 5.573 ms | 4.417 ms | 0.79x | 1403 |
| 8192 | 15.47 ms | 18.40 ms | **1.19x** | 1852 |
| 32768 | 96.00 ms | 400.3 ms | 4.17x | 2580 |
| 65536 | 236.3 ms | 1082 ms | 4.58x | 2953 |
| 262144 | 868.2 ms | about 17 s | about 20x | 3373 |

The cost goes as **N^1.24** fitted over two and a half decades, against 1.11 for
exactly N log N and 2.0 for direct summation, and the tree depth grows from 6 to
11 levels over the same range, which is log N almost exactly. Direct summation at
262144 particles is extrapolated from its own N^2 scaling rather than measured,
and is marked as such wherever it appears.

**The crossover is at about 6100 particles**, which is high compared with the
figures usually quoted for Barnes-Hut, and it is high for a good reason: the
opponent is an AVX2 kernel at four fifths of its hardware ceiling on eight cores
rather than a scalar loop. A crossover measured against a scalar single-threaded
direct kernel would have been several times lower and would have flattered the
tree.

**The interaction counter says something the timings cannot.** At 262144
particles the tree computes 78 times fewer interactions than direct summation and
is only 20 times faster, so a tree interaction costs about four times a direct
one:

| Solver | N | Interactions per second |
| --- | --- | --- |
| Direct | 65536 | 3.97 G |
| Tree | 65536 | 0.82 G |
| Tree | 262144 | 1.02 G |

Direct summation computes four pairs per AVX2 register from contiguous arrays
with no branch and no address arithmetic. A tree's cell term is a scalar
computation at the end of an unpredictable walk, preceded by the comparison that
accepted the cell and by the ones that rejected the cells above it. That factor
of four, rather than the asymptotics, is what a faster tree has to attack, and it
is the reason the GPU traversal was worth attempting at all.

The rest of the evaluation is cheap and measured: the Morton sort, the gather and
the build together come to under five per cent at the sizes where a tree is the
right choice, and the tree build itself is a fifth of one per cent at 262144
particles. That is the evidence for ADR-0022, which rebuilds the whole tree every
evaluation rather than maintaining one.

Accuracy against cost, and the reason quadrupole moments are an option rather
than a default:

| Target RMS error | Monopole only | With quadrupoles | Cheaper |
| --- | --- | --- | --- |
| 1e-2 | 11.3 ms | not reachable at any legal angle | monopole |
| 1e-3 | 43.5 ms | 43.9 ms | neither |
| 1e-4 | 127 ms | 90 ms | quadrupole, by 1.4x |

The default opening angle of 0.5 sits almost exactly where the two are equal,
which is why ADR-0024 makes the moments available and leaves them off.

## The GPU, running the same direct summation

The integrated Arc 130V runs the direct kernel as a backend behind the same
solver interface, in SYCL, sharing physical memory with the CPU. Single
precision, against all eight CPU cores running AVX2:

| N | GPU | CPU | Speedup | Staging | Gflop/s |
| --- | --- | --- | --- | --- | --- |
| 1024 | 0.178 ms | 0.119 ms | 0.67x | 1.0% | 117 |
| 2048 | 0.351 ms | 0.334 ms | 0.95x | 0.5% | 253 |
| 8192 | 1.555 ms | 4.511 ms | 2.90x | 0.3% | 890 |
| 65536 | 75.39 ms | 308.5 ms | **4.09x** | 0.2% | 1139 |
| 131072 | 297.1 ms | not timed | | 0.1% | **1174** |

**The GPU overtakes the CPU at about 2200 particles and loses below it.** A
kernel launch costs roughly 150 microseconds on this runtime, and a thousand-body
simulation should stay on the CPU. Saying so is more useful than quoting the
speedup at the size where it flatters the device.

At 1174 Gflop/s the kernel is at 42 per cent of the device's multiply-add ceiling
and 17.5 per cent of its divide ceiling, so unlike the CPU kernel it is bound by
neither. **What takes the remainder has not been identified**, and the two
candidates are named rather than guessed between: the two work-group barriers per
tile, and the local memory traffic the tiling trades global traffic for.
Distinguishing them needs a tile size sweep and sub-group instrumentation that no
phase has run.

The staging column is host memory to host memory and exists only because this
driver does not report `usm_system_allocations`. It falls from 1.0 per cent of an
evaluation to 0.1 as N grows, which is the one-over-N behaviour ADR-0027 predicts,
and there is no host-to-device copy anywhere: that claim is demonstrated by test
and the demonstration is described in [the validation report](validation.md).

## Walking the tree on the GPU

Direct summation suits a GPU because every work-item does identical work in
identical order. A tree walk does not, and a GPU does not execute work-items
independently: it executes them in sub-groups of 32 that share one instruction
pointer, so a walk written as though each work-item were a thread has every lane
switched off while the others finish the nodes it did not need.

So the sub-group walks together, with one node index for all 32 lanes and a lane
that has accepted a cell masked out until the group leaves that subtree.

| N | Independent walk | Coherent walk | Speedup | Nodes visited per lane |
| --- | --- | --- | --- | --- |
| 16384 | 5.309 ms | 1.529 ms | **3.47x** | 1.25x more |
| 131072 | 43.12 ms | 13.06 ms | **3.30x** | 1.24x more |
| 1048576 | 343.6 ms | 116.9 ms | **2.94x** | 1.19x more |

**The coherent walk steps through about 25 per cent more nodes and takes under a
third of the time**, because the nodes it adds are ones the hardware was already
executing under a divergence mask. A second session gives 3.96x, 2.92x and 2.51x,
so the honest claim is about three times rather than any one of those figures,
and that is the claim the project makes.

The same trade decides the sub-group width. At 262144 particles a width of 32
beats a width of 16 by 1.78 times on the traversal while visiting 6 per cent more
nodes, because sharing each node read, each acceptance test and each escape
pointer over twice as many lanes is worth far more than the redundancy costs. If
a width of 64 existed on this part it would be worth measuring.

Against the two solvers it has to beat, in single precision:

| N | GPU tree | CPU tree | GPU direct | vs CPU tree | vs GPU direct |
| --- | --- | --- | --- | --- | --- |
| 16384 | 2.921 ms | 25.92 ms | 7.073 ms | 8.87x | 2.42x |
| 65536 | 13.88 ms | 145.8 ms | 79.18 ms | 10.51x | **5.71x** |
| 262144 | 59.80 ms | 732.0 ms | not timed | **12.24x** | |
| 2097152 | 603.5 ms | not timed | not timed | | |

**The largest tractable configuration is about 2.1 million particles**, at 603 ms
per force evaluation and 87.5 MiB of shared memory, and nothing about the device
stopped it there: the traversal is 41 per cent of the evaluation and the memory
is a two-hundredth of what the runtime reports. What stopped it is that a session
has to finish.

## Which solver at which size

Taking the four solvers together, on this machine, the fastest thing the project
can do at a given particle count:

| Particles | Fastest | Because |
| --- | --- | --- |
| Below about 2200 | CPU direct, AVX2 on eight cores | A kernel launch costs more than the arithmetic saves |
| 2200 to about 9000 | GPU direct | Enough arithmetic to amortise the launch, not enough structure for a tree |
| Above about 9000 | GPU tree | The traversal is three times faster coherent, and the approximation is cheap |
| Above about 6100, without a GPU | CPU tree | The crossover against CPU direct summation |

The tree has to reach half again the size to be worth using on the GPU as on the
CPU, which is the expected direction: direct summation is the kernel a GPU is
built for and a tree walk is the kernel it is not.

Two qualifications belong beside that table. The crossovers are for a Plummer
sphere, and a tree's cost depends on how clustered the configuration is. And they
are for eight threads, since the two algorithms do not parallelise equally well.

## The renderer

Drawing alone, at 1280 by 720, with additive blending into a floating-point
target and a tone mapping pass:

| Particles | Frames per second |
| --- | --- |
| 20 000 | 5020 |
| 200 000 | 740 |
| 1 000 000 | 126 |

A live run draws and integrates in the same loop, and there the solver is the
limit rather than the renderer: 141 frames a second at ten thousand particles, 56
at twenty thousand and 34 at thirty thousand, at which point the renderer is
drawing three thousand frames a second and waiting. **The galaxy collision runs
live at thirty thousand particles above thirty frames a second, and a recorded
run plays back at a million.**

## The bottleneck now

Every phase in this project ended by naming what it had made slow, and the
current answer is not on the GPU.

**The Morton sort on the host is 40 to 46 per cent of every GPU tree
evaluation**, roughly level with the device traversal above 262144 particles and
more than ten times the tree build it exists to enable. A phase spent making the
traversal three times faster has succeeded in making the sort the bottleneck.
That is the most useful thing the measurement says and it is the first thing to
fix: a device radix sort would move both the time and the ceiling on the largest
tractable configuration, which is set by the host rather than by the device.

Three smaller items are open and each is recorded where it was found: the direct
kernel's tile size and its two barriers per tile have never been swept, the tree's
leaf capacity has never been swept although its leaves average 9.5 particles
against a capacity of 32, and the GPU direct kernel's reproducible throughput dip
at 16384 particles has not been explained.

## How much of this reproduces

The honest answer has two halves and both are part of the result.

**For configurations this part can sustain, the figures reproduce.** The
double-precision session behind the kernel table has interquartile ranges from
1.3 to 5.0 per cent on every row, its thermal canary moved 5.2 per cent across
nine minutes of load, and its repeat row, the first configuration measured again
at the end, came back within 0.4 per cent. The tree scaling session's canary
moved 1.7 per cent, the steadiest the project has recorded. Three GPU sessions
minutes apart give 4.09x, 4.20x and 4.06x at 65536 particles. The GPU traversal
reproduces to within 7 per cent at every size across two sessions taken forty
minutes apart.

**For sustained eight-thread vector work, they do not always.** The session
before the one quoted produced 22.4 ms for the eight-thread AVX2 row rather than
13.8, with a 27 per cent spread, a 34 per cent drift inside the row, and a canary
that spiked to 2.09 times its starting duration. That is the part hitting its
power limit partway through a measurement. The difference between the two
sessions is roughly the difference between a machine that had been under load for
the previous hour and one that had not, and both of them are this machine.

The harness reported all of that rather than averaging it away, which is what it
is for, and the remedy in every case was the one ADR-0019 prescribes: let the
machine idle, repeat the session, and quote the repeat, saying so.

One more pattern is worth carrying: **in the GPU tree solver, every anomalous row
in either session is anomalous in its host share and not in its device walk.**
The part that runs on the GPU is fast and steady; the part that runs on the host
is half the time and all of the variance.

## Reproducing all of it

```
cmake --preset release
cmake --build --preset release
./build/release/benchmarks/orrery_roofline 8192 21
./build/release/benchmarks/orrery_threading_scaling 8192 11
./build/release/benchmarks/orrery_tree_scaling
```

and, for the GPU, with the oneAPI compiler:

```
cmake --preset sycl-single-precision -DCMAKE_CXX_COMPILER=icx-cl
cmake --build --preset sycl-single-precision
./build/sycl-single-precision/benchmarks/orrery_sycl_direct
./build/sycl-single-precision/benchmarks/orrery_sycl_tree
```

Let the machine idle for several minutes first. The last of those is by a wide
margin the longest session the project runs, since it drives the GPU and all
eight cores in turn at sizes where one evaluation is most of a second, on a part
where both share one package and one power budget. It takes an optional largest
particle count, so `orrery_sycl_tree 262144` runs a few minutes rather than half
an hour.

The programs write their tables as CSV into the current directory. The copies
under [`performance/`](performance/) are the sessions these documents quote:
`roofline.csv`, `tree_scaling.csv`, `tree_accuracy.csv`, `sycl_tree_scaling.csv`
and `sycl_tree_divergence.csv`, beside the roofline plot the harness draws.

## What is not measured

The bandwidth ceilings are for the CPU alone, taken before this project asked the
GPU for anything. What a kernel sees with the GPU busy is lower by an amount
nothing here measures.

The arithmetic ceilings are for the whole part with work stealing across both
kinds of core. They are not per-core figures and cannot be divided by eight.

The comparison against the divide and square root ceiling assumes those two
operations are the only contended resource. A kernel reaching four fifths of a
ceiling is evidence that the ceiling is the right one; it is not proof that no
other limit binds alongside it.

Nothing here is a measurement of a physical result. These are the costs of
computing a force evaluation, and how many of them a scientific question needs is
a different subject.
