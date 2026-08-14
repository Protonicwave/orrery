/**
 * The design, sampled and stepped, drawn into the drawing.
 *
 * The preview is the same C++ the native binary runs, compiled to WebAssembly
 * and stepped in a Worker (ADR-0051), given the design as the text of a
 * configuration file. So what appears is a sample of the configuration rather
 * than an impression of it, and if the drawing and the preview disagree then one
 * of them is wrong and worth finding.
 *
 * It is drawn in the drawing's own projection, into a canvas behind the
 * drawing's surface, rather than through the instrument's renderer. Two reasons,
 * and the first is the one that decides it: the drawing dimensions lengths, so
 * the particles have to be at the same scale as the dimension lines or the two
 * halves of the picture would contradict each other, and a perspective camera
 * cannot be at the same scale as a plan. The second is that a technical drawing
 * is not a photograph: the plate's tone mapping, exposure and additive blending
 * exist to show a field of light, and what this needs to show is where the
 * particles are.
 */

import { parseConfiguration } from '../config/parse';
import { type BrowserRun, browserPlan } from '../solver/configure';
import type { FramePositions, FrameSource } from '../trajectory/client';
import { type Design, writeDesign } from './design';
import { primaryCount } from './elements';

/**
 * The most particles a preview draws.
 *
 * A design is edited by dragging, and a preview that takes a second to restart
 * is one that is watched rather than used. Three thousand particles steps fast
 * enough on one thread with the scalar kernel to keep up with a slider, and it
 * is enough of a disc to show the shape, the tilt and the beginning of the
 * structure. It is not enough to show the spiral arms the published run has, and
 * the interface says so rather than letting the picture make the claim.
 */
export const PREVIEW_PARTICLES = 3000;

/** The plan for a preview of `design`, respecting the module's own limit. */
export function previewPlan(design: Design, particleLimit: number): BrowserRun {
  const configuration = parseConfiguration(writeDesign(design));
  const requested = design.kind === 'kepler' ? undefined : design.count;
  return browserPlan(
    configuration,
    requested,
    design.steps,
    Math.min(particleLimit, PREVIEW_PARTICLES),
  );
}

/**
 * Where the second galaxy's particles start, or null when there is only one.
 *
 * The sampler documents the ordering: the primary's particles come first, then
 * the secondary's. That is part of its interface rather than an accident of its
 * implementation, which is what makes it safe to read here, and it is what lets
 * the preview draw the two galaxies apart. The instrument cannot do this from a
 * published trajectory, because a trajectory records positions and not which
 * component a particle was sampled into; the editor can, because the design
 * says.
 */
export function secondaryFrom(design: Design, count: number): number | null {
  if (design.kind !== 'galaxy-collision') return null;
  return primaryCount({ ...design, count });
}

/** How the preview is placed on the paper, shared with the drawing exactly. */
export interface View {
  /** Pixels to a model unit. */
  readonly scale: number;
  /** Where the model's origin sits, in pixels from the top left. */
  readonly originX: number;
  readonly originY: number;
  readonly width: number;
  readonly height: number;
}

/**
 * A canvas the frames are painted into.
 *
 * Every draw walks the three component arrays and writes one pixel per particle.
 * Nothing is allocated on the path: the same two colours are set once a frame,
 * the loop indexes typed arrays, and no object is made per particle. The rule is
 * the render loop's rule (section 2.5 of the budget), and it holds here for the
 * same reason: a preview that stutters while a slider is being dragged is a
 * preview that gets turned off.
 */
export class PreviewSurface {
  private readonly context: CanvasRenderingContext2D | null;
  private view: View = { scale: 1, originX: 0, originY: 0, width: 0, height: 0 };
  private split: number | null = null;
  private ratio = 1;

  constructor(private readonly canvas: HTMLCanvasElement) {
    // No alpha. The paper behind it is opaque, and a context without a transparent
    // layer is the cheaper one to composite.
    this.context = canvas.getContext('2d', { alpha: false });
  }

  /** The two colours the particles are drawn in, read from the design tokens. */
  private primaryInk = '#e6e2da';
  private secondaryInk = '#c8913f';
  private paper = '#000000';

  setInk(primary: string, secondary: string, paper: string): void {
    this.primaryInk = primary;
    this.secondaryInk = secondary;
    this.paper = paper;
  }

  /**
   * Follow the drawing's placement, and size the backing store to the display.
   *
   * The canvas is sized in device pixels and scaled back in CSS pixels, so a
   * particle is a pixel on the screen rather than a soft square on a display
   * that has more of them than the layout does.
   */
  place(view: View, devicePixelRatio: number): void {
    this.view = view;
    this.ratio = devicePixelRatio;
    const width = Math.round(view.width * devicePixelRatio);
    const height = Math.round(view.height * devicePixelRatio);
    if (this.canvas.width !== width) this.canvas.width = width;
    if (this.canvas.height !== height) this.canvas.height = height;
  }

  /** Which particle the second galaxy starts at, or null for one galaxy. */
  setSplit(split: number | null): void {
    this.split = split;
  }

  clear(): void {
    const context = this.context;
    if (context === null) return;
    context.fillStyle = this.paper;
    context.fillRect(0, 0, this.canvas.width, this.canvas.height);
  }

  /**
   * Paint one frame.
   *
   * The z coordinate is dropped rather than projected, because the view is a
   * plan and a plan is an orthographic projection along z. A disc seen at an
   * inclination is foreshortened by that alone, which is the same ellipse the
   * drawing's construction lines show, and the two therefore register.
   */
  draw(frame: FramePositions): void {
    const context = this.context;
    if (context === null) return;

    this.clear();

    const { scale, originX, originY } = this.view;
    const ratio = this.ratio;
    const kx = scale * ratio;
    const ox = originX * ratio;
    const oy = originY * ratio;
    const size = Math.max(1, Math.round(ratio));
    const split = this.split ?? frame.x.length;

    context.fillStyle = this.primaryInk;
    const first = Math.min(split, frame.x.length);
    for (let index = 0; index < first; index += 1) {
      context.fillRect(
        ox + (frame.x[index] as number) * kx,
        oy - (frame.y[index] as number) * kx,
        size,
        size,
      );
    }

    if (first < frame.x.length) {
      context.fillStyle = this.secondaryInk;
      for (let index = first; index < frame.x.length; index += 1) {
        context.fillRect(
          ox + (frame.x[index] as number) * kx,
          oy - (frame.y[index] as number) * kx,
          size,
          size,
        );
      }
    }
  }
}

/** How fast a completed preview replays, in frames a second. */
const REPLAY_RATE = 30;

/**
 * Which frame a preview should be showing.
 *
 * While the run is being computed the preview follows the head of it, because
 * what a person editing a design wants to see is the newest state rather than a
 * recording of an older one. Once the run is finished there is nothing to
 * follow, so it replays what was computed, on a loop, at a rate that does not
 * depend on how fast the machine drew the last one.
 */
export function previewFrame(
  source: FrameSource,
  elapsedMilliseconds: number,
): FramePositions | undefined {
  const available = source.available;
  if (available === 0) return undefined;
  if (source.status !== 'complete') return source.frame(available - 1);

  const index = Math.floor((elapsedMilliseconds / 1000) * REPLAY_RATE) % available;
  return source.frame(index);
}
