# The tree traversal on the GPU, and what coherence is worth inside it

Measured on the machine [the performance report](../performance.md) describes.

The short version, in five findings.

**Walking the tree one sub-group at a time is between 2.9 and 3.5 times faster
than letting each work-item walk on its own**, and it achieves that while
stepping through about 1.2 times *more* nodes. That is the phase's result and
the reason it is stated in both directions: the extra nodes are ones the
hardware was already executing under a divergence mask, so the coherent walk
spends time the independent walk was wasting.

**The largest tractable configuration on this machine is about 2.1 million
particles**, at 603 ms per force evaluation and 87.5 MiB of shared memory. What
stops it there is not the device.

**The GPU tree solver is 10 to 12 times faster than the CPU tree solver** above
about 60000 particles, and it overtakes the GPU direct kernel of Phase 9 at
about 9000. At 262144 particles it is 12.2 times faster than the same tree on
all eight CPU cores.

**The limit is now the Morton sort on the host**, which is 40 to 46 per cent of
every evaluation across the whole table and is the single largest line item
above 8192 particles. The device traversal, the thing this phase set out to make
fast, is down to 41 per cent.

And **the sub-group width matters, in the direction that is not obvious**. The
wider group of 32 lanes beats the narrower group of 16 by a factor of 1.78 on
the traversal, despite visiting 6 per cent more nodes, because sharing the cost
of each node over twice as many lanes buys far more than the redundancy costs.

## The machine and the device

Taken with the `sycl-single-precision` preset, compiled by the oneAPI DPC++
compiler 2025.1.1, on the CPU described in [`roofline.md`](roofline.md) and the
device described in [`sycl_direct.md`](sycl_direct.md).

| Property | Value |
| --- | --- |
| Device | Intel Arc 130V GPU, Xe2 |
| Runtime | oneAPI Unified Runtime over Level Zero |
| Driver | 1.15.37669 |
| Compute units | 64 |
| Sub-group widths offered | 16 and 32 |
| Default sub-group width | 32 |
| Work-group size | 256 |
| Configuration | Plummer sphere, seed 20260811, softening 0.05 |
| Parameters | Opening angle 0.5, leaf capacity 32, monopole only |
| Protocol | 11 timed trials after a settling warm-up, median reported, 3 s cool-down |

Session of 2026-08-11 13:22, single precision. Two sessions were taken and this
is the second; the section at the end says which figures reproduce between them
and why the first is not the one quoted.

The device offers only two sub-group widths. Intel Xe hardware has historically
supported eight as well, and this driver does not report it, so the sweep below
has two rows rather than three. That is what the runtime says rather than an
omission.

## The divergence mitigation

This is what the phase exists to measure, and section 7 of the implementation
plan asks for it to be measured rather than assumed. Both traversals run over
the same tree, built by the same host code from the same configuration, and
compute the same sum: `tests/solvers/sycl_tree_solver_test.cpp` requires their
interaction counters to be equal, so what follows compares two control flows
rather than two algorithms.

| N | Independent walk | Coherent walk | Speedup | Independent visits per particle | Coherent visits per particle | Redundancy |
| --- | --- | --- | --- | --- | --- | --- |
| 16384 | 5.309 ms | 1.529 ms | **3.47x** | 909 | 1135 | 1.25x |
| 131072 | 43.12 ms | 13.06 ms | **3.30x** | 1905 | 2360 | 1.24x |
| 1048576 | 343.6 ms | 116.9 ms | **2.94x** | 2775 | 3312 | 1.19x |

Both times are the device traversal alone, since the host half is identical
between them.

The two rightmost columns are the cost, and they are worth reading carefully.
`visits` counts the nodes one work-item stepped through. For the independent
walk that is its own walk and nothing else, so 909 nodes per particle at 16384
is the algorithm's own figure. For the coherent walk it is the whole sub-group's
union, counted once per lane, so 1135 means that walking with 31 neighbours made
each lane see 25 per cent more nodes than it needed.

**The coherent walk does 25 per cent more work and takes under a third of the
time.** That is only paradoxical if a GPU is thought of as executing work-items
independently. It does not. The lanes of the independent walk are already
stepping through their union, because they share an instruction pointer and the
hardware masks off whichever lanes did not want the branch it is executing. What
the independent walk lacks is any way to use that time. The coherent walk reads
each node once for 32 lanes rather than 32 times from 32 addresses, evaluates
one acceptance test over 32 targets, and follows one escape pointer.

The redundancy columns are identical between the two sessions to the last digit,
which they should be: the number of nodes a sub-group visits is a property of
the tree and of the width, not of the machine's temperature. The speedup falls
with N in both sessions, from 3.47 to 2.94 here and from 3.96 to 2.51 in the
repeat, and the redundancy falls with it, from 1.25 to 1.19. Both move for the
same reason: a deeper tree means neighbouring particles agree about more of it
near the root and disagree about more near their own leaves, so the union grows
more slowly than the walks do, while the leaf summations, which are perfectly
coherent under neither scheme, take a larger share of the time.

## Scaling, and where it stops

Twelve sizes, from 1024 to 2097152 particles, which is three decades.

| N | Total | Spread | Device walk | Host share | Exponent | ms / (N log N) | Cells per particle | Visits per particle |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1024 | 0.281 ms | 3.7% | 0.227 ms | 13.0% | - | 2.75e-05 | 45 | 165 |
| 2048 | 0.550 ms | 6.8% | 0.403 ms | 21.3% | 0.97 | 2.44e-05 | 113 | 294 |
| 4096 | 1.027 ms | 4.3% | 0.704 ms | 27.6% | 0.90 | 2.09e-05 | 212 | 461 |
| 8192 | 1.702 ms | 3.6% | 1.208 ms | 43.6% | 0.73 | 1.60e-05 | 384 | 733 |
| 16384 | 2.921 ms | 2.8% | 1.483 ms | 48.0% | 0.78 | 1.27e-05 | 639 | 1135 |
| 32768 | 6.590 ms | 3.8% | 3.376 ms | 47.5% | 1.17 | 1.34e-05 | 925 | 1566 |
| 65536 | 13.88 ms | 6.3% | 6.599 ms | 49.7% | 1.07 | 1.32e-05 | 1216 | 1952 |
| 131072 | 45.25 ms | 15.2% | 15.33 ms | 77.1% | 1.71 | 2.03e-05 | 1529 | 2360 |
| 262144 | 59.80 ms | 4.1% | 27.44 ms | 55.1% | 0.40 | 1.27e-05 | 1764 | 2644 |
| 524288 | 129.9 ms | 2.2% | 54.80 ms | 57.5% | 1.12 | 1.30e-05 | 2156 | 3077 |
| 1048576 | 415.0 ms | 6.7% | 121.8 ms | 69.4% | 1.68 | 1.98e-05 | 2352 | 3312 |
| 2097152 | 603.5 ms | 8.7% | 241.8 ms | 59.0% | 0.54 | 1.37e-05 | 2459 | 3425 |

The exponent is the local slope against the row above. Fitted between the first
row and the last it is **1.01**, against 1.10 for exactly N log N over the same
range and 2 for direct summation.

Falling *below* N log N is not a claim about the algorithm and should not be read
as one. It is the small rows: at 1024 particles the evaluation is 0.28
milliseconds, of which most is the fixed cost of launching a kernel at all, so
the first few rows are flatter than the method is. Fitted from 65536 upwards,
where that cost is negligible, the exponent is **1.09** against 1.08 for exactly
N log N. That is the range where the asymptotic claim applies, and there it
holds closely.

The `ms / (N log N)` column says the same thing without the differencing. It
falls by half over the first four rows, where the tree has barely any structure
to exploit, and from 16384 upwards it is flat to within 4 per cent once the two
anomalous rows below are set aside.

That is a closer fit to N log N than the CPU solver's 1.24 in
[`barnes_hut.md`](barnes_hut.md), and the reason is not flattering to this phase.
The CPU figure rose above N log N because the interactions per particle grow
faster than log N, which the last two columns show they still do here. What has
changed is that half of each evaluation is now a sort, which is genuinely
N log N, and it dilutes the traversal's steeper growth.

**Two rows should not be quoted.** The 131072 row carries a 15.2 per cent
interquartile spread and a 77.1 per cent host share against 50 per cent either
side of it, and the 1048576 row a 69.4 per cent host share. Both are host-side
stalls rather than anything the device did: their device walk times, 15.33 and
121.8 ms, sit exactly where the neighbouring rows and the repeat session put
them, and it is the sort that inflated. They are left as they were measured, and
the exponent column shows what they do to the fit, throwing 1.71 and then 0.40
across a step where every clean pair of rows gives about 1.1.

### Where one evaluation goes

| N | Nodes | Depth | Sort | Gather | Build | Node staging | Device walk | Scatter |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1024 | 171 | 7 | 10.3% | 0.6% | 1.5% | 0.1% | 87.0% | 0.5% |
| 4096 | 568 | 8 | 23.2% | 0.8% | 2.7% | 0.1% | 72.4% | 0.9% |
| 8192 | 1064 | 8 | 37.8% | 1.2% | 3.2% | 0.1% | 56.4% | 1.3% |
| 16384 | 2171 | 9 | 39.9% | 1.4% | 4.3% | 0.1% | 52.0% | 2.3% |
| 65536 | 7924 | 10 | 40.8% | 1.6% | 3.8% | 0.1% | 50.3% | 3.4% |
| 262144 | 31411 | 11 | 46.4% | 2.6% | 3.3% | 0.1% | 44.9% | 2.7% |
| 524288 | 62242 | 11 | 46.4% | 3.9% | 3.3% | 0.1% | 42.5% | 3.8% |
| 2097152 | 246602 | 12 | 44.3% | 5.9% | 3.2% | 0.1% | 41.0% | 5.5% |

**The Morton sort is the largest single cost in the solver.** It is 40 to 46 per
cent of every evaluation above 8192 particles, roughly level with the device
traversal from 262144 upwards, and more than ten times the tree build it exists
to make possible. That was not the expected answer and it is the most useful
thing this table says: a phase spent making the traversal three times faster has
succeeded in making the sort the bottleneck.

The tree build itself is 1.5 to 4.3 per cent throughout, which is what ADR-0028
assumed when it left construction on the host. That assumption holds, and the
assumption a device radix sort would overturn is a different one. ADR-0028
records it as the first thing to do to this solver rather than as something
Phase 10 was going to reach.

The node staging that ADR-0030 introduced is **0.1 per cent** of an evaluation at
every size, which turns the claim that the conversion is O(number of nodes)
rather than O(N) into a measurement.

The gather and the scatter together are 1.1 per cent at 1024 particles and 11.4
per cent at 2097152. They are not the staging step ADR-0027 describes: they are
the reordering the tree algorithm needs anyway, written straight into and out of
shared memory, so the GPU tree solver pays nothing for staging that the CPU tree
solver does not also pay. That is the dividend ADR-0028 predicted, and the way to
read it in this table is that there is no column for staging at all.

### The largest row

| Property | Value |
| --- | --- |
| Particles | 2,097,152 |
| Tree nodes | 246,602, depth 12 |
| Shared memory | 87.5 MiB, of which 7.5 MiB is the tree |
| Evaluation | 603.5 ms, of which 241.8 ms is the device |

**Nothing about the device stopped the table here.** 87.5 MiB of shared memory
against the 16 GB the runtime reports, and a traversal that is 41 per cent of
the evaluation. What stopped it is that a session has to finish: at two million
particles a row of eleven trials is thirteen seconds and the next size up would
be a minute, on top of a Morton sort that is nearly half of it.

So the honest statement of the largest tractable particle count is in two parts.
Two million particles is comfortable, at about 0.6 seconds per force evaluation,
which is roughly seventeen hours of wall time for a hundred thousand steps. The
ceiling above that is set by host memory and by the host sort, not by the GPU,
and a device sort would move it.

## Against the two solvers it has to beat

A GPU tree is only worth having where it beats both the CPU tree of Phase 8 and
the GPU direct kernel of Phase 9. Both are timed here on the same
configurations, in the same session, on one thermal state of the machine.

| N | GPU tree | CPU tree | GPU direct | vs CPU tree | vs GPU direct |
| --- | --- | --- | --- | --- | --- |
| 1024 | 0.281 ms | 0.805 ms | 0.180 ms | 2.86x | 0.64x |
| 2048 | 0.550 ms | 2.441 ms | 0.355 ms | 4.44x | 0.65x |
| 4096 | 1.027 ms | 4.737 ms | 0.690 ms | 4.61x | 0.67x |
| 8192 | 1.702 ms | 10.67 ms | 1.548 ms | 6.27x | 0.91x |
| 16384 | 2.921 ms | 25.92 ms | 7.073 ms | 8.87x | **2.42x** |
| 32768 | 6.590 ms | 63.97 ms | 22.19 ms | 9.71x | 3.37x |
| 65536 | 13.88 ms | 145.8 ms | 79.18 ms | 10.51x | **5.71x** |
| 131072 | 45.25 ms | 337.0 ms | 296.2 ms | 7.45x | 6.55x |
| 262144 | 59.80 ms | 732.0 ms | - | **12.24x** | - |

Every total includes everything an evaluation does, so the tree solvers are
charged for their sort and their build and the direct solver for its staging.
The 131072 row is the anomalous one noted above and its two ratios are
correspondingly depressed; the repeat session gives 11.69x and 10.50x there.

Against the CPU tree solver the speedup rises to 10 to 12 times and then
flattens, which is what a solver whose host half is half its time must do: the
traversal is far more than twelve times faster, and the sort is not faster at
all. It runs on the same eight cores in both solvers.

Against the GPU direct kernel the crossover falls between 8192, where the tree
is still losing at 0.91, and 16384, where it wins by 2.42. Interpolating
logarithmically puts it at **about 8800 particles**, and the repeat session puts
it at 9000. It is worth setting beside the crossover on the CPU in
[`barnes_hut.md`](barnes_hut.md), which is 6100: the tree has to reach half again
the size to be worth using on the GPU as on the CPU, because direct summation is
the kernel a GPU is built for and a tree walk is the kernel it is not.

Taking the three solvers together, the fastest thing this project can do at a
given size is now the CPU direct kernel below about 2200 particles, the GPU
direct kernel from there to about 9000, and the GPU tree solver above that. The
first of those figures comes from [`sycl_direct.md`](sycl_direct.md).

## The sub-group width

The width is the granularity of the coherence: how many targets agree to walk the
tree together. It decides both how much redundancy the scheme costs and how much
divergence it removes, so it is swept rather than argued about. Measured at
262144 particles.

| Width | Device walk | Total | Spread | Visits per particle |
| --- | --- | --- | --- | --- |
| Compiler's choice | 27.61 ms | 61.99 ms | 4.0% | 2644 |
| 16 | 49.03 ms | 80.29 ms | 5.4% | 2494 |
| 32 | 27.48 ms | 60.74 ms | 6.1% | 2644 |

**The wider group wins by 1.78 times on the traversal while visiting 6 per cent
more nodes**, which is the same trade as the coherent-against-independent
comparison and resolves the same way. Doubling the width doubles the number of
lanes that each node read, each acceptance test and each escape-pointer chase is
shared over, and that is worth far more than the 6 per cent of extra nodes it
costs. It also suggests the mechanism has not run out: if a width of 64 existed
on this part it would be worth measuring.

The first row is the configuration a run actually uses, where the width is left
to the compiler. It lands within half a per cent of the explicit 32, which
confirms that the device's reported default of 32 is what it in fact compiles
for. That row is included precisely so that the named widths can be read against
the default rather than only against each other.

## Accuracy

| N | Worst relative error | Root mean square |
| --- | --- | --- |
| 4096 | 2.03e-2 | 2.03e-3 |
| 16384 | 1.34e-2 | 1.76e-3 |
| 65536 | 7.12e-3 | 1.48e-3 |

Measured against `solvers/reference_kernel.hpp`, which sums the same softened
force law with compensation in double precision. Identical between the two
sessions to every digit printed, which is what a deterministic solver on a fixed
seed should produce and is worth checking rather than assuming.

**This is the opening angle's error and not the device's**, and the difference
matters enough to be stated rather than left to a tolerance. These figures match
[`barnes_hut.md`](barnes_hut.md)'s for the same opening angle, because the GPU
walk and the CPU walk sum the same terms in the same order. The evidence for that
claim is not this table: it is that `tests/solvers/sycl_tree_solver_test.cpp`
requires the two solvers' interaction counters to be equal, which says the device
opened the same cells and summed the same pairs, and then requires their
accelerations to agree to rounding rather than to the size of the approximation.

For comparison, the GPU direct kernel's root mean square error at 65536 particles
is 3.7e-6, which is 400 times better. Approximating the physics costs far more
accuracy than computing it in single precision does, and that remains the single
most useful accuracy fact in this project.

## What Phase 9 left here, and what became of it

[`sycl_direct.md`](sycl_direct.md) deferred two things to this phase, and it is
better to say what happened to them than to let them lapse quietly.

The first was the direct kernel's throughput dip at 16384 particles, which that
document attributed to the padded launch geometry fitting the device least well
and left for the traversal work. **It was not investigated.** The subject here is
the tree traversal, and a measurement that spreads into every question it passes
finishes none of them. The dip does reproduce in this
session's `GPU direct` column, where 16384 particles take 7.07 ms against 1.55 at
8192, a factor of 4.6 for a factor of 4 in interactions.

The second was that the direct kernel sits at 42 per cent of the device's
multiply-add ceiling and 17.5 per cent of its divide ceiling, so something else
takes the remainder, and identifying it would need sub-group-level
instrumentation. Phase 10 built some of that instrumentation for a different
kernel, and what it found is suggestive rather than conclusive: this device
rewards wider sub-groups strongly, 1.78 times for a doubling from 16 to 32 lanes,
which says that per-lane overheads rather than arithmetic are what the traversal
pays for. The direct kernel's analogous knob is its tile size, and the two
work-group barriers per tile are the other candidate
[`sycl_direct.md`](sycl_direct.md) names. Neither was swept. A phase that returns
to the direct kernel should sweep both, and it now has a benchmark shaped like
the one that would do it.

## A note on the sanitisers

Nothing changed here and the gap is inherited rather than introduced, but a
phase that adds a kernel should say what is not instrumenting it.

[`sycl_direct.md`](sycl_direct.md) records that the address sanitiser cannot be
combined with `-fsycl` at all, because every translation unit is compiled for the
device as well as the host and the device target rejects the flag. The address
sanitiser therefore covers this project except its SYCL translation units, and
the traversal added in this phase is one of them. It was run and passes 210 tests
in the configuration that excludes them, and the undefined-behaviour sanitiser
still does not build on this machine for the reason
[`barnes_hut.md`](barnes_hut.md) gives, which is a C runtime mismatch in its own
runtime library rather than anything in this repository.

What stands in for it here is narrower than for the direct kernel and worth
stating. The traversal indexes a node array with 32-bit integers converted from
64-bit host indices, and the conversion is bounds-checked at the evaluation
boundary rather than trusted (ADR-0030). The padded launch is allocated rather
than merely tolerated, so the lanes with no particle of their own read and write
inside the allocation. And the strongest check is not a sanitiser at all: the
device walk's interaction counters are required to equal the CPU walk's exactly,
which a traversal that ran off the end of the node array or skipped a subtree
would fail immediately.

## How much of this reproduces

Two sessions were taken, forty minutes apart, and the second is the one quoted.

**The first is not quoted because its thermal canary reached 83.5 per cent across
the whole session**, which is the largest this project has recorded outside the
failed Phase 9 session that led to the three-second cool-down. Its scaling rows
were bracketed by a canary of -4.4 per cent and were internally sound, but the
tables measured after them were not: its sub-group width sweep put the
compiler's-choice row at a 52 per cent interquartile spread and 27.6 ms against
29.5 for the explicit width of 32 it should have matched. A row like that is a
thermal state reported as a result. The remedy is the one
[`roofline.md`](roofline.md) prescribes and [`sycl_direct.md`](sycl_direct.md)
used: let the machine idle, repeat the session, and quote the repeat. The second
session's canary is **8.4 per cent across the whole session** and 14.7 across the
scaling rows.

The stronger evidence is agreement between the two, and the interesting part is
which columns agree.

**The device traversal reproduces to within 7 per cent at every size.**

| N | Session 1 walk | Session 2 walk | Difference |
| --- | --- | --- | --- |
| 16384 | 1.476 ms | 1.483 ms | 0.5% |
| 65536 | 6.639 ms | 6.599 ms | 0.6% |
| 262144 | 27.52 ms | 27.44 ms | 0.3% |
| 524288 | 57.90 ms | 54.80 ms | 5.4% |
| 1048576 | 114.0 ms | 121.8 ms | 6.8% |
| 2097152 | 239.6 ms | 241.8 ms | 0.9% |

**The host half is where the variance lives.** Every row that either session
reports as anomalous is anomalous in its host share and not in its device walk:
session 2's 131072 row has a 77 per cent host share against 50 per cent for its
neighbours while its walk time sits within 17 per cent of session 1's, and
session 1's 2097152 row carries a 47 per cent spread while its walk agrees with
session 2's to within 1 per cent.

That is worth more than a reproducibility statement, because it is the same
conclusion the time-split table reaches by a different route. The part of this
solver that runs on the GPU is fast and steady. The part that runs on the host is
half the time and all of the noise.

The divergence speedups agree in shape rather than in value: 3.47, 3.30 and 2.94
here against 3.96, 2.92 and 2.51 in the first session. Both sessions have the
speedup between 2.5 and 4.0 and falling with N, and both have the redundancy
identical to the digit, since it is a property of the tree rather than of the
machine. A claim that the mitigation is worth "about three times" is supported;
a claim that it is worth 3.47 times at 16384 particles is not, and is not made.

Reproduce with:

```
cmake --preset sycl-single-precision -DCMAKE_CXX_COMPILER=icx-cl
cmake --build --preset sycl-single-precision
./build/sycl-single-precision/benchmarks/orrery_sycl_tree
```

The program takes an optional largest size, so `orrery_sycl_tree 262144` runs a
session of a few minutes rather than half an hour. It writes
`sycl_tree_scaling.csv` and `sycl_tree_divergence.csv` into the current
directory, and the copies in this directory are from the second session.

Let the machine idle for several minutes first. This is by a wide margin the
longest and hottest session this project runs: it drives the GPU and all eight
CPU cores in turn, at sizes where a single evaluation is most of a second, on a
part where both share one package and one power budget.
