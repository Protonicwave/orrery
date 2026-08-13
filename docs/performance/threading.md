# CPU threading on a hybrid processor

Measured on the machine [the performance report](../performance.md) describes.

The short version: dividing the force loop into eight equal shares leaves the
performance cores idle for 64.5 per cent of every force evaluation. Letting idle
workers take work from busy ones brings that to 8.8 per cent and the wall time
down by 42 per cent. The reason is that this machine's cores are not alike, and
the size of the difference is measured here rather than assumed.

## The machine and the method

| Property | Value |
| --- | --- |
| CPU | Intel Core Ultra 5 238V, 4 Lion Cove plus 4 Skymont, 8 threads |
| Scalar type | `double` |
| Build | `RelWithDebInfo`, Clang 22, no explicit vectorisation |
| Power | Mains, Windows power mode set to best performance |
| Configuration | Plummer sphere, 8192 particles, seed 20260810, softening 0.05 |
| Repeats | 11 timed evaluations after 2 discarded, median reported |

Reproduce with:

```
cmake --preset release
cmake --build --preset release
./build/release/benchmarks/orrery_threading_scaling 8192 11
```

The kernel is the Phase 5 direct solver unchanged. Threading divides its outer
loop over target particles and nothing else, so every configuration below runs
identical arithmetic in an identical order and the results are bit for bit the
same in all of them. That is asserted by test, not assumed
(`tests/solvers/parallel_direct_solver_test.cpp`).

**These timings are indicative and the methodology is provisional.** Phase 7
owns the benchmark harness, with the statistical treatment and the explicit
handling of thermal throttling that this program does not have. The spread
column below, the difference between the fastest and slowest of eleven trials as
a fraction of the median, runs from 5 to 36 per cent. That is too noisy to quote
a timing to three figures and is stated rather than smoothed over. What is *not*
noisy is everything derived from the per-worker instrumentation: idle fractions
and work distributions reproduced to within a percentage point across runs, and
they are where the conclusions come from.

## The hardware asymmetry

The same evaluation, single-threaded, pinned to one core of each kind:

| Core | Median per evaluation | Relative |
| --- | --- | --- |
| Performance (Lion Cove) | 207 ms | 1.00 |
| Efficiency (Skymont) | 448 ms | 2.17 |

A performance core does this work 2.17 times as fast as an efficiency core.
Every result below follows from that number.

It also sets the ceiling. Eight cores of which four run at half speed are worth

    4 x 1 + 4 x (1 / 2.17) = 5.84

times one performance core, not eight. A speedup should be read against 5.84
rather than against 8, and quoting the fraction of that limit rather than the
raw figure is the rule every speed in this project is stated by.

This is also a warning about baselines. The first version of this measurement
let the operating system place the single-threaded baseline and reported
speedups above ten on an eight-core machine, because the baseline had landed on
an efficiency core. A baseline that is not pinned is not a baseline.

## Static against dynamic

Eight workers, pinned so that idle time can be attributed to a class of core:

| Scheme | Median | Speedup | Fraction of the 5.84 limit | Idle, all | Idle, P cores | Idle, E cores |
| --- | --- | --- | --- | --- | --- | --- |
| Static shares | 71.7 ms | 2.88x | 49% | 39.8% | **64.5%** | 15.1% |
| Work stealing | 41.7 ms | 4.95x | 85% | 7.7% | **8.8%** | 6.6% |

And unpinned, which is how the project ships:

| Scheme | Median | Speedup | Fraction of the limit | Idle, all |
| --- | --- | --- | --- | --- |
| Static shares | 47.2 ms | 4.38x | 75% | 20.3% |
| Work stealing | 39.5 ms | 5.23x | 90% | 3.5% |

Idle time here is the share of worker time not spent running the kernel, summed
over workers. It includes thread wake-up and the wait at the end of a region for
slower workers to finish, deliberately: those costs are real and a definition
that excluded them would flatter the scheduler.

## Where the time goes

The aggregates above are explained entirely by the per-worker records. Busy and
idle are totals over the eleven measured evaluations.

Static shares, pinned:

| Worker | Core | Particles | Busy | Idle | Particles per ms busy |
| --- | --- | --- | --- | --- | --- |
| 0 | performance | 11264 | 294 ms | 521 ms | 38.4 |
| 1 | performance | 11264 | 290 ms | 525 ms | 38.9 |
| 2 | performance | 11264 | 286 ms | 528 ms | 39.4 |
| 3 | performance | 11264 | 285 ms | 529 ms | 39.5 |
| 4 | efficiency | 11264 | 814 ms | 1 ms | 13.8 |
| 5 | efficiency | 11264 | 714 ms | 100 ms | 15.8 |
| 6 | efficiency | 11264 | 638 ms | 176 ms | 17.7 |
| 7 | efficiency | 11264 | 600 ms | 214 ms | 18.8 |

Every worker is given the same 11264 particles, which is the scheme working as
designed. Worker 4 is busy for essentially the whole run and idle for one
millisecond in eight hundred: it is the critical path, and the other seven
workers, including all four performance cores, are waiting on it. The
performance cores spend nearly twice as long idle as working.

Work stealing, pinned:

| Worker | Core | Particles | Busy | Idle | Particles per ms busy |
| --- | --- | --- | --- | --- | --- |
| 0 | performance | 15360 | 448 ms | 27 ms | 34.3 |
| 1 | performance | 15808 | 429 ms | 47 ms | 36.9 |
| 2 | performance | 16064 | 429 ms | 47 ms | 37.5 |
| 3 | performance | 16256 | 428 ms | 47 ms | 38.0 |
| 4 | efficiency | 6592 | 446 ms | 29 ms | 14.8 |
| 5 | efficiency | 6656 | 440 ms | 36 ms | 15.1 |
| 6 | efficiency | 6592 | 444 ms | 31 ms | 14.8 |
| 7 | efficiency | 6784 | 446 ms | 29 ms | 15.2 |

The busy times are now equal to within four per cent across cores of both kinds,
which is what balanced means. The particle counts are not equal and should not
be: the performance cores took 70.5 per cent of the work between them, a ratio
of 2.38 to one against the efficiency cores.

That ratio is the result worth pausing on. Nothing in the scheduler knows which
cores are fast. It was never told the 2.17 measured above, and it holds no
weights, no calibration and no topology. The distribution is what falls out of
letting a worker that has run out take more, and it lands within nine per cent
of the hardware ratio measured independently. This is the argument for measuring
the balance continuously rather than baking a weight in: a weight would have to
be right, and this cannot be wrong.

## Thread scaling

Work stealing, unpinned, against the pinned single performance core:

| Workers | Median | Speedup |
| --- | --- | --- |
| 1 | 198 ms | 1.05x |
| 2 | 99.8 ms | 2.07x |
| 3 | 67.3 ms | 3.07x |
| 4 | 61.4 ms | 3.37x |
| 5 | 48.9 ms | 4.23x |
| 6 | 45.1 ms | 4.58x |
| 7 | 43.1 ms | 4.79x |
| 8 | 40.3 ms | 5.13x |

Close to linear to three workers, then flattening as the scheduler starts
placing work on efficiency cores. The curve is the hardware's shape rather than
the scheduler's: the marginal core is worth about 0.46 of a performance core
once the performance cores are occupied, and the increments from five workers
onwards, 0.86, 0.35, 0.21 and 0.34, average close to that against a noise floor
the one-worker row puts at about five per cent.

## Why pinning is off by default

Pinning makes static partitioning substantially worse, 71.7 ms against 47.2 ms.
That is not an artefact. Unpinned, the operating system notices a thread has
finished and migrates the next runnable one onto the free core, which recovers
part of what equal shares threw away. Pinning removes that, leaving the scheme's
own behaviour exposed, which is exactly why the per-class figures are taken from
pinned runs.

Both Windows and Linux understand hybrid topologies and place threads with
information a fixed assignment does not have, so the pools in this project leave
affinity to the operating system unless a caller is measuring per-core-class
behaviour. Work stealing is close to indifferent, 41.7 ms pinned against 39.5 ms
free, which is what a scheme that balances itself should look like.

## What this does not measure

The kernel is scalar. Phase 7 adds explicit AVX2 vectorisation, which will make
every core faster and may well change the ratio between the two kinds, since
Lion Cove and Skymont differ more in vector throughput than in scalar. The
balance the scheduler finds should follow the new ratio without changes, and
that is a prediction this document is making and Phase 7 can check.

Nothing here is a roofline. These are wall times against a single-core baseline,
not against the memory bandwidth or floating-point limits of the part, which
Phase 7 measures and after which every performance claim in this project is
expressed as a fraction of them.

The numbers were taken on a laptop under sustained load, which throttles. The
program pauses between configurations and reports the spread of every median so
that a drifting measurement is visible, but it does not control for temperature.
Phase 7 addresses that directly.
