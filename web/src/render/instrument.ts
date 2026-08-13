/**
 * The render loop, and the controls that drive it.
 *
 * This is the imperative half of the client. It owns the canvas, the camera and
 * the animation frame, it reads its source's frames directly, and it never
 * touches React: a value that changes sixty times a second must not go through
 * a virtual DOM, which is the decision Phase 2's store was split in two for.
 *
 * The source is an interface rather than the trajectory reader, because there
 * are now two things that produce frames in order: a published run being
 * decoded, and a run being integrated in a Worker by the WebAssembly build of
 * the solver. Neither is visible from here.
 *
 * ## What the loop is allowed to allocate
 *
 * Nothing. Every array, matrix and record it uses is made in the constructor
 * and written into thereafter. That is checked rather than asserted: the
 * profile in `docs/instrument.md` is a minute of playback with no growth in the
 * heap. The two exceptions are inside the WebGPU backend and are the API's
 * rather than this file's; `render/webgpu.ts` says which and why.
 *
 * ## The controls are the native viewer's
 *
 * Drag to turn, right-drag to pan, scroll to zoom, `-` and `=` for exposure,
 * space to pause, `r` to reframe. The sensitivities are the constants from
 * `src/viz/viewer_window.cpp` rather than numbers chosen to feel similar, so a
 * drag of a given fraction of the window turns both viewers by the same angle.
 * Every one of them has a keyboard equivalent, because a control that only a
 * mouse can reach is a control half the people who arrive cannot use.
 */

import type { FrameState } from '../state/store';
import type { FrameSource } from '../trajectory/client';
import { OrbitCamera } from './camera';
import type { PlateFurniture } from './furniture';
import type { Positions, Renderer, RenderSettings } from './renderer';

/**
 * Two and a half turns of azimuth across the width of the window, and a
 * ninth of the distance per notch of the wheel. Both from
 * `src/viz/viewer_window.cpp`.
 */
const ORBIT_PER_SCREEN = 15.7;
const ZOOM_PER_NOTCH = 0.11;

/** What one press or one repeat of the exposure keys does, as the native does. */
const EXPOSURE_STEP = 1.03;

/** What one press of an arrow key turns the camera by, in radians. */
const ORBIT_PER_KEY = 0.08;

/** What one press of Page Up or Page Down does to the distance. */
const ZOOM_PER_KEY = 0.9;

/**
 * Trajectory frames played per second of wall clock.
 *
 * The native viewer draws one trajectory frame per display frame, which ties
 * the speed of the run to the refresh rate of the monitor. Here the two are
 * separated, so the published run takes the same seven seconds on a sixty hertz
 * panel and a hundred and forty-four hertz one. Sixty is the rate the native
 * viewer reaches on the machine the figures were measured on, so the two look
 * the same on the hardware the demonstration was written for.
 */
const FRAMES_PER_SECOND = 60;

/** How quickly the frame-rate readout follows the truth. */
const RATE_SMOOTHING = 0.1;

/**
 * How often the chrome is told what instant is being shown, in milliseconds.
 *
 * Ten times a second. The clock and the step readout are values a person reads,
 * and a person cannot read sixty of them a second; sampling here is what keeps
 * a rendered instant off the React path without leaving the chrome showing an
 * instant the plate has passed.
 */
const SAMPLE_INTERVAL = 100;

export interface InstrumentOptions {
  readonly container: HTMLElement;
  readonly canvas: HTMLCanvasElement;
  readonly renderer: Renderer;
  /**
   * The frames to play: a published trajectory being read, or a run being
   * integrated in the browser. The loop does not distinguish them, which is the
   * point of the interface.
   */
  readonly source: FrameSource;
  /** The mutable record the loop writes its per-frame numbers into. */
  readonly frame: FrameState;
  readonly settings: RenderSettings;
  readonly furniture: PlateFurniture | null;
  /** Told when a control changes something the chrome owns. */
  readonly onExposure: (exposure: number) => void;
  readonly onPlaying: (playing: boolean) => void;
  /** Told what instant is on the plate, ten times a second and no more. */
  readonly onSample: (time: number, step: number) => void;
}

/** What the plate's furniture reads. Implemented by the instrument itself. */
export interface PlateReadout {
  readonly camera: OrbitCamera;
  readonly settings: RenderSettings;
  readonly frame: FrameState;
  readonly viewMatrix: Float32Array;
  readonly backend: string;
  readonly particles: number;
  /** Pixels one model unit covers at the distance of the target. */
  readonly pixelsPerUnit: number;
  /** Frames decoded so far, and how many there will be. */
  readonly available: number;
  readonly frames: number;
}

export class Instrument implements PlateReadout {
  readonly camera = new OrbitCamera();

  settings: RenderSettings;
  playing = false;

  /**
   * The frames being played.
   *
   * Replaceable, through `play`, because the instrument outlives the choice
   * between a published run and one being integrated here. Rebuilding it to
   * change that would mean tearing down and restarting a graphics device to
   * answer a button.
   */
  private source: FrameSource;

  private readonly options: InstrumentOptions;
  private readonly viewProjection = new Float32Array(16);
  readonly viewMatrix = new Float32Array(16);

  /** Reused every frame. Its three arrays are the trajectory's, not copies. */
  private readonly positions: {
    count: number;
    x: Float32Array;
    y: Float32Array;
    z: Float32Array;
  } = {
    count: 0,
    x: new Float32Array(0),
    y: new Float32Array(0),
    z: new Float32Array(0),
  };

  /** Where playback has reached, in frames, which need not be a whole number. */
  private position = 0;
  private lastTime = 0;
  private sampledAt = 0;
  private request = 0;
  private framed = false;
  private width = 0;
  private height = 0;

  private dragging: 'orbit' | 'pan' | null = null;
  private pointer = 0;
  private lastX = 0;
  private lastY = 0;

  constructor(options: InstrumentOptions) {
    this.options = options;
    this.settings = options.settings;
    this.source = options.source;

    const { canvas } = options;
    canvas.tabIndex = 0;
    canvas.setAttribute('role', 'img');

    canvas.addEventListener('pointerdown', this.onPointerDown);
    canvas.addEventListener('pointermove', this.onPointerMove);
    canvas.addEventListener('pointerup', this.onPointerUp);
    canvas.addEventListener('pointercancel', this.onPointerUp);
    canvas.addEventListener('wheel', this.onWheel, { passive: false });
    canvas.addEventListener('contextmenu', this.onContextMenu);
    canvas.addEventListener('keydown', this.onKeyDown);
  }

  get backend(): string {
    return this.options.renderer.description;
  }

  /** The per-frame numbers, which the loop writes and the furniture reads. */
  get frame(): FrameState {
    return this.options.frame;
  }

  get particles(): number {
    return this.positions.count;
  }

  get available(): number {
    return this.source.available;
  }

  get frames(): number {
    return this.source.facts?.frames ?? 0;
  }

  /**
   * Draw a different sequence of frames from the beginning.
   *
   * The transport goes back to the start and the camera is framed again on the
   * first frame that arrives, because the new source is a different run and the
   * distance that suited the old one need not suit it.
   */
  play(source: FrameSource): void {
    if (source === this.source) return;
    this.source = source;
    this.position = 0;
    this.framed = false;
    this.positions.count = 0;
  }

  get pixelsPerUnit(): number {
    const visible = 2 * this.camera.distance * Math.tan(this.camera.fieldOfView / 2);
    return visible === 0 ? 0 : this.height / visible;
  }

  /** Where the transport is, in frames. */
  get frameIndex(): number {
    return Math.floor(this.position);
  }

  /** Move the transport. Clamped to what has been decoded. */
  seek(index: number): void {
    const highest = Math.max(0, this.available - 1);
    this.position = Math.min(Math.max(index, 0), highest);
  }

  /**
   * Move the transport to a moment in model time.
   *
   * The interval between frames is taken from the first two rather than from
   * the configuration's stride, because the trajectory is what is being played
   * and a configuration that has been edited since the run would be a second
   * answer to the same question. Frames are written at a fixed stride, so two
   * of them are enough to know where all of them are.
   *
   * Returns whether the frame for that moment had arrived. An address can name
   * a moment near the end of a run that is still being read, and the caller
   * needs to know whether to ask again when more of it has.
   */
  seekTime(time: number): boolean {
    const { source } = this;
    const index = source.indexAt(time);
    if (index < 0) {
      // The spacing between frames is not yet known. The first frame is the
      // only one there is to be at.
      const first = source.frame(0);
      if (first === undefined) return false;
      this.seek(0);
      return time <= first.time;
    }

    this.seek(index);
    return index <= this.available - 1;
  }

  start(): void {
    if (this.request === 0) {
      this.lastTime = performance.now();
      this.request = requestAnimationFrame(this.tick);
    }
  }

  stop(): void {
    if (this.request !== 0) cancelAnimationFrame(this.request);
    this.request = 0;
  }

  dispose(): void {
    this.stop();
    const { canvas } = this.options;
    canvas.removeEventListener('pointerdown', this.onPointerDown);
    canvas.removeEventListener('pointermove', this.onPointerMove);
    canvas.removeEventListener('pointerup', this.onPointerUp);
    canvas.removeEventListener('pointercancel', this.onPointerUp);
    canvas.removeEventListener('wheel', this.onWheel);
    canvas.removeEventListener('contextmenu', this.onContextMenu);
    canvas.removeEventListener('keydown', this.onKeyDown);
  }

  /**
   * Point the camera at the particles and stand back far enough to see them.
   *
   * Three times the root-mean-square radius about the centre of the particles,
   * which is what `frame_particles` in `apps/orrery_view.cpp` does and for the
   * reason it gives: a bounding sphere is set by whichever particle the
   * encounter flung furthest and would show a galaxy the size of a full stop.
   */
  reframe(): void {
    const { count, x, y, z } = this.positions;
    if (count === 0) return;

    let centreX = 0;
    let centreY = 0;
    let centreZ = 0;
    for (let index = 0; index < count; index += 1) {
      centreX += x[index] as number;
      centreY += y[index] as number;
      centreZ += z[index] as number;
    }
    centreX /= count;
    centreY /= count;
    centreZ /= count;

    let squared = 0;
    for (let index = 0; index < count; index += 1) {
      const dx = (x[index] as number) - centreX;
      const dy = (y[index] as number) - centreY;
      const dz = (z[index] as number) - centreZ;
      squared += dx * dx + dy * dy + dz * dz;
    }

    this.camera.target.x = centreX;
    this.camera.target.y = centreY;
    this.camera.target.z = centreZ;
    this.camera.distance = 3 * Math.sqrt(squared / count);
  }

  private readonly tick = (now: number): void => {
    this.request = requestAnimationFrame(this.tick);

    const elapsed = Math.min((now - this.lastTime) / 1000, 0.25);
    this.lastTime = now;

    this.measure();
    this.advance(elapsed);
    this.render();

    const { frame } = this.options;
    if (elapsed > 0) {
      const instant = 1 / elapsed;
      frame.framesPerSecond += (instant - frame.framesPerSecond) * RATE_SMOOTHING;
    }

    this.options.furniture?.update(this);

    if (now - this.sampledAt >= SAMPLE_INTERVAL) {
      this.sampledAt = now;
      this.options.onSample(frame.time, frame.step);
    }
  };

  /**
   * Follow the size of the container, in device pixels.
   *
   * Read from the element rather than from a resize observer's callback, which
   * would deliver a size at a moment that is not this one and would have to be
   * stored somewhere until the frame that used it.
   */
  private measure(): void {
    const ratio = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(this.options.container.clientWidth * ratio));
    const height = Math.max(1, Math.round(this.options.container.clientHeight * ratio));
    if (width === this.width && height === this.height) return;

    this.width = width;
    this.height = height;
    this.options.canvas.width = width;
    this.options.canvas.height = height;
    this.options.renderer.resize(width, height);
  }

  private advance(elapsed: number): void {
    const available = this.available;
    if (available === 0) return;

    if (this.playing) {
      this.position += elapsed * FRAMES_PER_SECOND;
      const last = available - 1;
      if (this.position > last) {
        // Loop only once the whole run has been read. Wrapping at the edge of
        // what has arrived would play the opening seconds over and over while
        // the rest of the file was still coming, which reads as a stuck run.
        this.position =
          this.source.status === 'complete' ? this.position % available : last;
      }
    }

    const frame = this.source.frame(Math.floor(this.position));
    if (frame === undefined) return;

    this.positions.count = frame.x.length;
    this.positions.x = frame.x;
    this.positions.y = frame.y;
    this.positions.z = frame.z;
    this.options.frame.time = frame.time;
    this.options.frame.step = frame.step;

    if (!this.framed) {
      this.framed = true;
      this.reframe();
    }
  }

  private render(): void {
    const { renderer } = this.options;
    const aspect = this.height === 0 ? 1 : this.width / this.height;
    this.camera.view(this.viewMatrix);
    this.camera.viewProjection(aspect, renderer.depthRange, this.viewProjection);
    renderer.draw(this.positions as Positions, this.viewProjection, this.settings);
  }

  private readonly onPointerDown = (event: PointerEvent): void => {
    if (event.button !== 0 && event.button !== 2) return;
    // Shift and the left button do what the right button does, for a trackpad
    // and for anyone who cannot hold a modifier and drag at once.
    this.dragging = event.button === 2 || event.shiftKey ? 'pan' : 'orbit';
    this.pointer = event.pointerId;
    this.lastX = event.clientX;
    this.lastY = event.clientY;
    this.options.canvas.setPointerCapture(event.pointerId);
    this.options.canvas.focus();
    event.preventDefault();
  };

  private readonly onPointerMove = (event: PointerEvent): void => {
    if (this.dragging === null || event.pointerId !== this.pointer) return;

    // Both deltas are divided by the width, as the native viewer divides them,
    // so that a diagonal drag turns and tilts by the same amount.
    const span = this.options.canvas.clientWidth || 1;
    const dx = (event.clientX - this.lastX) / span;
    const dy = (event.clientY - this.lastY) / span;
    this.lastX = event.clientX;
    this.lastY = event.clientY;

    if (this.dragging === 'orbit') {
      // Dragging right turns the camera to the left around the subject, which
      // is what makes the subject appear to follow the cursor.
      this.camera.orbit(-dx * ORBIT_PER_SCREEN, dy * ORBIT_PER_SCREEN);
    } else {
      this.camera.pan(-dx, dy);
    }
  };

  private readonly onPointerUp = (event: PointerEvent): void => {
    if (event.pointerId !== this.pointer) return;
    this.dragging = null;
    if (this.options.canvas.hasPointerCapture(event.pointerId)) {
      this.options.canvas.releasePointerCapture(event.pointerId);
    }
  };

  private readonly onContextMenu = (event: Event): void => {
    // Right-dragging is a camera control here, and a menu appearing halfway
    // through one would leave the drag unfinished.
    event.preventDefault();
  };

  private readonly onWheel = (event: WheelEvent): void => {
    event.preventDefault();
    // A wheel reports pixels, lines or pages. A notch is about a hundred pixels
    // and about three lines, and a positive delta is away from the reader,
    // which is away from the subject.
    const perUnit = event.deltaMode === 1 ? 1 / 3 : event.deltaMode === 2 ? 1 : 1 / 100;
    const notches = -event.deltaY * perUnit;
    this.camera.zoom(1 - notches * ZOOM_PER_NOTCH);
  };

  private readonly onKeyDown = (event: KeyboardEvent): void => {
    if (event.ctrlKey || event.altKey || event.metaKey) return;

    switch (event.key) {
      case 'ArrowLeft':
        this.camera.orbit(-ORBIT_PER_KEY, 0);
        break;
      case 'ArrowRight':
        this.camera.orbit(ORBIT_PER_KEY, 0);
        break;
      case 'ArrowUp':
        this.camera.orbit(0, ORBIT_PER_KEY);
        break;
      case 'ArrowDown':
        this.camera.orbit(0, -ORBIT_PER_KEY);
        break;
      case 'PageUp':
        this.camera.zoom(ZOOM_PER_KEY);
        break;
      case 'PageDown':
        this.camera.zoom(1 / ZOOM_PER_KEY);
        break;
      case '-':
        this.options.onExposure(this.settings.exposure / EXPOSURE_STEP);
        break;
      case '=':
      case '+':
        this.options.onExposure(this.settings.exposure * EXPOSURE_STEP);
        break;
      case 'r':
        this.reframe();
        break;
      case ' ':
        this.options.onPlaying(!this.playing);
        break;
      default:
        return;
    }
    event.preventDefault();
  };
}
