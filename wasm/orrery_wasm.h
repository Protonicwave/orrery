#ifndef ORRERY_WASM_H
#define ORRERY_WASM_H

/**
 * The whole of what crosses the boundary between this project and a browser.
 *
 * The simulator is C++ with templates, spans, exceptions and a class hierarchy
 * in every interface it has. None of that can be described in a WebAssembly
 * module's exports, which are functions taking and returning numbers. Something
 * has to stand between the two, and this file is that something: about a dozen
 * functions, each of which takes numbers and pointers and nothing else.
 *
 * It is a C interface rather than a generated binding over the library for the
 * reason ADR-0051 sets out at length. The short version is that a binding
 * generator would carry the whole class hierarchy into the module, and this
 * boundary is not a class hierarchy: it is create, step, read, destroy.
 *
 * ## The rules this interface follows
 *
 * Nothing here throws. The library beneath does, at its configuration boundary,
 * because a file with three mistakes in it should say so. Every entry point
 * below catches, and reports the failure as a value: a null handle, a zero
 * count, or a false. `orrery_last_error` then holds the sentence. That is the
 * project's rule about errors at boundaries (a value) and inside them (an
 * exception), applied to a boundary that cannot carry an exception at all.
 *
 * Every pointer this file returns points into the module's own memory and stays
 * valid until the next call that could change it. A caller reading one after
 * destroying the simulation it came from is reading freed memory, and the
 * interface cannot stop it: that is the price of a boundary made of numbers.
 *
 * Positions and masses are copied out into a buffer the caller owns rather than
 * exposed as pointers into the particle store. The store holds `double` in this
 * build and the renderer wants `float`, so a copy happens either way, and doing
 * it here means it happens once, in compiled code, rather than in a loop on the
 * thread that is also drawing.
 *
 * ## Counts
 *
 * Every count is a 32-bit signed integer. The module is built for wasm32, so
 * there is no larger index available, and the particle count is capped far
 * below the point at which the distinction matters. Signed rather than
 * unsigned, so that a negative return can mean "it did not work" without
 * needing a sentinel that is also a legitimate count.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** An opaque handle to a run in progress. */
typedef struct OrrerySimulation OrrerySimulation;

/**
 * The revision of this interface.
 *
 * Read by the JavaScript that loads the module and compared against the number
 * it was written for. A module and a page are deployed together and should
 * never disagree, but a browser holding a cached module from a previous deploy
 * is exactly the case where they will, and a mismatch found at load time is
 * better than one found as a wrong answer.
 */
int orrery_abi_version(void);

/**
 * Bytes in the scalar type this module was built with: 8 for double, 4 for
 * float.
 *
 * The same fact the trajectory format records in its header, reported here for
 * the same reason: what precision a number was computed in is part of the
 * number.
 */
int orrery_scalar_size(void);

/**
 * The largest particle count this module will create.
 *
 * A browser tab is a shared resource and the direct solver is quadratic, so a
 * configuration asking for more than this is refused rather than run slowly.
 * The client reads this and states it, rather than carrying its own copy of the
 * number.
 */
int orrery_particle_limit(void);

/** The largest number of steps a single `orrery_advance` call will take. */
int orrery_step_limit(void);

/**
 * The last failure, as a sentence, or an empty string if nothing has failed.
 *
 * Cleared at the start of every call that can fail, so it always describes the
 * most recent one rather than the first one ever.
 */
const char* orrery_last_error(void);

/**
 * Start a run from the text of an `.orrery` configuration file.
 *
 * The text rather than a struct of numbers, so that the browser and the
 * command line read the same document with the same parser. A configuration
 * edited in the browser, previewed here and downloaded is then the same file
 * the native binary runs, which is the property the editor depends on.
 *
 * Returns null on a configuration that will not parse, will not run, or asks
 * for more particles than `orrery_particle_limit`. `orrery_last_error` says
 * which.
 *
 * The output section of the configuration is ignored. There is nowhere in a
 * browser tab to write a trajectory to, and a run that silently wrote nothing
 * where it was asked to write something would be worse than one that says so:
 * `orrery_report` says so.
 */
OrrerySimulation* orrery_create(const char* configuration);

/** Release a run. Null is accepted and does nothing. */
void orrery_destroy(OrrerySimulation* simulation);

/**
 * What the module did with the configuration that it was not asked to do.
 *
 * The executor it forced to serial, the outputs it ignored, the solver it fell
 * back to. Shown on screen rather than logged, because the whole point of the
 * browser build is that it must never look like it is claiming the native
 * figures.
 */
const char* orrery_report(const OrrerySimulation* simulation);

/** The name of the solver in use: `direct`, `barnes-hut`. */
const char* orrery_solver_name(const OrrerySimulation* simulation);

/** The name of the innermost kernel: `scalar`, `avx2`. */
const char* orrery_kernel_name(const OrrerySimulation* simulation);

int orrery_particle_count(const OrrerySimulation* simulation);

/** The step counter, which is the run's clock. */
int orrery_step_index(const OrrerySimulation* simulation);

double orrery_timestep(const OrrerySimulation* simulation);

/** The simulated time, `step * timestep`. */
double orrery_time(const OrrerySimulation* simulation);

/**
 * Advance by `steps` steps.
 *
 * Returns the number taken, which is `steps` or, if something threw, however
 * many completed before it did. A caller that gets fewer than it asked for
 * reads `orrery_last_error` and stops.
 *
 * `steps` is bounded by `orrery_step_limit` so that one call cannot occupy the
 * Worker for an unbounded time. A caller wanting more calls again.
 */
int orrery_advance(OrrerySimulation* simulation, int steps);

/**
 * Copy the positions into `out` as three contiguous blocks of
 * `orrery_particle_count` floats: every x, then every y, then every z.
 *
 * Component blocks rather than triples, which is how the particle store holds
 * them (ADR-0040) and how the renderer's instance buffers want them. `out` must
 * have room for three times the particle count.
 *
 * Returns false, and writes nothing, if the handle is null.
 */
int orrery_read_positions(const OrrerySimulation* simulation, float* out);

/**
 * Copy the masses into `out`, which must have room for the particle count.
 *
 * Read once, after creating the run. They do not change while it runs, which is
 * the same reason the trajectory format keeps them in its header.
 */
int orrery_read_masses(const OrrerySimulation* simulation, float* out);

/** How many doubles `orrery_measure` writes. */
#define ORRERY_MEASUREMENT_COUNT 11

/**
 * Measure the conserved quantities and write them into `out`.
 *
 * Eleven doubles, in the order the columns of a diagnostics file carry them:
 *
 *   0  kinetic energy
 *   1  potential energy
 *   2  total energy
 *   3  relative energy error, against the first measurement of this run
 *   4  virial ratio, 2T/|U|
 *   5  6  7   total linear momentum, x y z
 *   8  9 10   total angular momentum, x y z
 *
 * The same order and the same definitions as `sim/diagnostics_log.cpp` writes,
 * so a browser reading of a run and a file reading of one are the same reading.
 *
 * Costs a pass over every pair, as it does natively, so a caller measures every
 * few hundred steps rather than every step.
 *
 * Returns false, and writes nothing, if the handle is null.
 */
int orrery_measure(OrrerySimulation* simulation, double* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORRERY_WASM_H */
