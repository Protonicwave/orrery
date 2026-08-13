/// <reference lib="webworker" />

/**
 * The solver Worker: where the browser build of the simulator actually runs.
 *
 * It is off the main thread for the same reason the trajectory reader is. The
 * module integrates for tens of milliseconds at a time and cannot be
 * interrupted while a call is on its stack, so a step taken on the page's thread
 * would be a step during which nothing draws, nothing scrolls and no control
 * answers. ADR-0052 records the arrangement, including why there is one Worker
 * and not several.
 *
 * The loop is written as a chain of scheduled chunks rather than as one loop
 * that runs to the end. A Worker reads its message queue between tasks and never
 * during one, so a run that stepped to completion inside a single task could not
 * be stopped, and the stop button would be a control that does nothing until the
 * work it was meant to cancel has finished.
 */

import {
  lastError,
  loadOrrery,
  MEASUREMENT_COUNT,
  type OrreryModule,
  putString,
} from './module';
import type { FromWorker, StartMessage, ToWorker } from './protocol';

/**
 * How often the conserved quantities are measured, in recorded frames.
 *
 * Measuring costs a pass over every pair, which is the whole cost of a step for
 * the direct solver and several steps' worth for the tree. One frame in ten
 * keeps the diagnostics honest without turning the run into a measurement of
 * itself, and it is the same trade the native run makes when it sets a
 * diagnostics stride of a few hundred steps.
 */
const MEASURE_EVERY = 10;

/** Bytes in a float and in a double, named so the arithmetic below reads. */
const FLOAT = 4;
const DOUBLE = 8;

/** The state of the one run this Worker is capable of holding at a time. */
interface Run {
  readonly module: OrreryModule;
  readonly handle: number;
  readonly count: number;
  readonly stride: number;
  readonly steps: number;
  readonly frames: number;
  /** Staging buffers in the module's memory, allocated once and reused. */
  readonly positions: number;
  readonly measurement: number;
  index: number;
  stopped: boolean;
}

let run: Run | null = null;

function post(message: FromWorker, transfer: Transferable[] = []): void {
  self.postMessage(message, transfer);
}

function fail(message: string): void {
  post({ kind: 'failed', message, delivered: run?.index ?? 0 });
  release();
}

function release(): void {
  if (run === null) return;
  run.stopped = true;
  run.module._free(run.positions);
  run.module._free(run.measurement);
  run.module._orrery_destroy(run.handle);
  run = null;
}

/**
 * Send the positions as three transferable arrays.
 *
 * The module writes them into its own memory as one block of three component
 * arrays, and each is then copied out into an array of its own. That copy is
 * the only one: a view into the module's heap cannot be transferred, and it
 * would in any case be invalidated the moment the heap grew or the next step
 * overwrote it. ADR-0027's intent on a different device.
 */
function sendFrame(current: Run, stepMilliseconds: number): void {
  const { module, count } = current;
  if (module._orrery_read_positions(current.handle, current.positions) === 0) {
    fail(lastError(module));
    return;
  }

  const start = current.positions / FLOAT;
  const heap = module.HEAPF32;
  const x = new Float32Array(heap.subarray(start, start + count));
  const y = new Float32Array(heap.subarray(start + count, start + 2 * count));
  const z = new Float32Array(heap.subarray(start + 2 * count, start + 3 * count));

  post(
    {
      kind: 'frame',
      index: current.index,
      step: module._orrery_step_index(current.handle),
      time: module._orrery_time(current.handle),
      x,
      y,
      z,
      stepMilliseconds,
    },
    [x.buffer, y.buffer, z.buffer],
  );
}

function sendMeasurement(current: Run): void {
  const { module } = current;
  if (module._orrery_measure(current.handle, current.measurement) === 0) {
    fail(lastError(module));
    return;
  }

  const start = current.measurement / DOUBLE;
  const values = new Float64Array(
    module.HEAPF64.subarray(start, start + MEASUREMENT_COUNT),
  );
  post(
    {
      kind: 'measured',
      step: module._orrery_step_index(current.handle),
      time: module._orrery_time(current.handle),
      values,
    },
    [values.buffer],
  );
}

/**
 * Take one frame's worth of steps and report.
 *
 * Scheduled rather than looped, so the queue is read between frames. A timeout
 * of zero is a task boundary, which is all this needs: the work in one chunk is
 * already tens of milliseconds and adding to it would only make the run harder
 * to stop.
 */
function chunk(): void {
  const current = run;
  if (current === null || current.stopped) return;

  const { module, handle } = current;
  const remaining = current.steps - module._orrery_step_index(handle);
  const wanted = Math.min(current.stride, remaining);

  const began = performance.now();
  const taken = module._orrery_advance(handle, wanted);
  const elapsed = performance.now() - began;

  if (taken < wanted) {
    fail(lastError(module));
    return;
  }

  current.index += 1;
  sendFrame(current, taken === 0 ? 0 : elapsed / taken);
  if (run === null) return;

  if (current.index % MEASURE_EVERY === 0) {
    sendMeasurement(current);
    if (run === null) return;
  }

  if (module._orrery_step_index(handle) >= current.steps) {
    // The last measurement is taken whatever the stride, so that a finished run
    // always has a drift figure at its end as well as at its start. A native run
    // records the two ends of its diagnostics for the same reason.
    sendMeasurement(current);
    if (run === null) return;
    post({ kind: 'complete' });
    return;
  }

  setTimeout(chunk, 0);
}

/**
 * The module, loaded once and kept.
 *
 * Instantiating it costs a fetch and a compile, and a person who stops a run and
 * starts another should pay for neither a second time.
 */
let loaded: OrreryModule | null = null;

async function load(base: string): Promise<void> {
  loaded ??= await loadOrrery(base);
  post({
    kind: 'ready',
    particleLimit: loaded._orrery_particle_limit(),
    stepLimit: loaded._orrery_step_limit(),
    scalarSize: loaded._orrery_scalar_size(),
  });
}

function start(message: StartMessage): void {
  release();

  // Loaded by the message that preceded this one, which is what told this
  // Worker where the module is.
  const module = loaded;
  if (module === null) throw new Error('the solver module has not been loaded');

  const text = putString(module, message.configuration);
  const handle = module._orrery_create(text);
  module._free(text);
  if (handle === 0) throw new Error(lastError(module));

  const count = module._orrery_particle_count(handle);
  const positions = module._malloc(3 * count * FLOAT);
  const measurement = module._malloc(MEASUREMENT_COUNT * DOUBLE);
  if (positions === 0 || measurement === 0) {
    module._orrery_destroy(handle);
    throw new Error('the solver module is out of memory');
  }

  const stride = Math.max(1, Math.floor(message.stride));
  run = {
    module,
    handle,
    count,
    stride,
    steps: message.steps,
    frames: Math.floor(message.steps / stride) + 1,
    positions,
    measurement,
    index: 0,
    stopped: false,
  };

  // The masses widen to double here. The module holds them in the precision it
  // was built with and hands them over as floats, and the radial profile in the
  // data rail weights by mass in double, as every other reading in the client
  // does. One pass over the particles, once per run.
  //
  // They are read through the position buffer, which is three times the size
  // they need and is not holding anything yet.
  const masses = new Float64Array(count);
  if (module._orrery_read_masses(handle, positions) === 0) {
    throw new Error(lastError(module));
  }
  const base = positions / FLOAT;
  for (let index = 0; index < count; index += 1) {
    masses[index] = module.HEAPF32[base + index] as number;
  }

  post(
    {
      kind: 'started',
      count,
      timestep: module._orrery_timestep(handle),
      steps: run.steps,
      stride,
      frames: run.frames,
      solver: module.UTF8ToString(module._orrery_solver_name(handle)),
      kernel: module.UTF8ToString(module._orrery_kernel_name(handle)),
      report: module.UTF8ToString(module._orrery_report(handle)),
      masses,
    },
    [masses.buffer],
  );

  // The state at the first step, before anything has been integrated, so that a
  // run begins at the configuration it started from and the energy error has a
  // value to be relative to.
  sendFrame(run, 0);
  if (run === null) return;
  sendMeasurement(run);
  if (run === null) return;

  setTimeout(chunk, 0);
}

self.onmessage = (event: MessageEvent<ToWorker>): void => {
  const message = event.data;
  const caught = (error: unknown): void => {
    fail(error instanceof Error ? error.message : String(error));
  };

  switch (message.kind) {
    case 'load':
      load(message.base).catch(caught);
      break;
    case 'start':
      try {
        start(message);
      } catch (error) {
        caught(error);
      }
      break;
    case 'stop':
      release();
      break;
  }
};
