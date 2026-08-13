/**
 * The WebAssembly module, given a type and a name for each of its exports.
 *
 * Emscripten writes the loader beside the binary and the loader is untyped: the
 * module object it resolves to carries the exported functions as properties
 * whose names begin with an underscore. This file is the one place that knows
 * those names, so nothing else in the client reaches into an untyped object, and
 * the interface in `wasm/orrery_wasm.h` has exactly one transcription rather
 * than one per call site.
 *
 * The loader is imported at run time rather than bundled. It is compiler output
 * written into `public/solver` by the wasm preset, and it is served as a static
 * asset for the same reason the gallery's trajectories are: the client's build
 * should not have to be able to produce it. A checkout that has never run the
 * preset therefore has no module, the import fails, and the page says so.
 */

import { ORRERY_ABI_VERSION } from './version';

/**
 * The exports, as functions.
 *
 * A pointer is a number here, because that is what it is: an offset into the
 * module's memory. The aliases below say which numbers are offsets, since
 * nothing in the type system will.
 */
type Pointer = number;
type Handle = number;

export interface OrreryModule {
  _orrery_abi_version(): number;
  _orrery_scalar_size(): number;
  _orrery_particle_limit(): number;
  _orrery_step_limit(): number;
  _orrery_last_error(): Pointer;
  _orrery_create(configuration: Pointer): Handle;
  _orrery_destroy(simulation: Handle): void;
  _orrery_report(simulation: Handle): Pointer;
  _orrery_solver_name(simulation: Handle): Pointer;
  _orrery_kernel_name(simulation: Handle): Pointer;
  _orrery_particle_count(simulation: Handle): number;
  _orrery_step_index(simulation: Handle): number;
  _orrery_timestep(simulation: Handle): number;
  _orrery_time(simulation: Handle): number;
  _orrery_advance(simulation: Handle, steps: number): number;
  _orrery_read_positions(simulation: Handle, out: Pointer): number;
  _orrery_read_masses(simulation: Handle, out: Pointer): number;
  _orrery_measure(simulation: Handle, out: Pointer): number;
  _malloc(bytes: number): Pointer;
  _free(pointer: Pointer): void;

  /**
   * The module's memory, as typed views.
   *
   * Replaced by Emscripten whenever the heap grows, which is why every read
   * below takes the view fresh from the module rather than keeping one.
   */
  readonly HEAPF32: Float32Array;
  readonly HEAPF64: Float64Array;

  UTF8ToString(pointer: Pointer): string;
  stringToUTF8(text: string, pointer: Pointer, bytes: number): void;
  lengthBytesUTF8(text: string): number;
}

/** How many doubles `_orrery_measure` writes. `ORRERY_MEASUREMENT_COUNT`. */
export const MEASUREMENT_COUNT = 11;

/** Where in a measurement each quantity is. The order the header fixes. */
export const MEASUREMENT = {
  kineticEnergy: 0,
  potentialEnergy: 1,
  totalEnergy: 2,
  energyDrift: 3,
  virialRatio: 4,
  momentumX: 5,
  momentumY: 6,
  momentumZ: 7,
  angularMomentumX: 8,
  angularMomentumY: 9,
  angularMomentumZ: 10,
} as const;

type Factory = (options?: Record<string, unknown>) => Promise<OrreryModule>;

/**
 * Load and instantiate the module.
 *
 * @throws Error if the module is not there, will not instantiate, or was built
 * against a different revision of the interface than this file was written for.
 * The last of those is the one worth checking: a browser holding a cached
 * loader from a previous deploy is exactly when a page and a module disagree,
 * and a mismatch found here is better than one found as a wrong answer.
 */
export async function loadOrrery(base: string): Promise<OrreryModule> {
  // The path is built at run time and the bundler is told to leave it alone,
  // because the file is a static asset rather than a module in the graph.
  const url = new URL('solver/orrery.js', base).href;
  const loaded = (await import(/* @vite-ignore */ url)) as { default: Factory };
  const module = await loaded.default();

  const built = module._orrery_abi_version();
  if (built !== ORRERY_ABI_VERSION) {
    throw new Error(
      `the solver module implements interface ${built} and this page reads ${ORRERY_ABI_VERSION}`,
    );
  }

  return module;
}

/** Copy a string into the module's memory. The caller frees the result. */
export function putString(module: OrreryModule, text: string): Pointer {
  const bytes = module.lengthBytesUTF8(text) + 1;
  const pointer = module._malloc(bytes);
  if (pointer === 0) throw new Error('the solver module is out of memory');
  module.stringToUTF8(text, pointer, bytes);
  return pointer;
}

/** The last failure the module recorded, or a sentence saying it gave none. */
export function lastError(module: OrreryModule): string {
  const message = module.UTF8ToString(module._orrery_last_error());
  return message === '' ? 'the solver module stopped without saying why' : message;
}
