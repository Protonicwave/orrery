/**
 * One renderer interface, with the device behind it.
 *
 * ADR-0026 put the GPU behind the solver interface rather than in front of it,
 * so that a caller asks for an acceleration field and never learns which device
 * evaluated it. The same shape holds here for the same reason: the instrument
 * asks for a frame to be drawn, and whether that happened through WebGPU or
 * WebGL2 is a fact about the machine rather than about the picture. ADR-0046.
 *
 * The interface is narrow on purpose. Two backends have to be kept identical,
 * and every method on this interface is a place they can differ.
 *
 * ## What both backends draw
 *
 * The same two passes as `src/viz/point_renderer.cpp`: every particle as an
 * instanced sprite with a Gaussian falloff, blended additively into a
 * half-float target with no depth test, then one full-screen pass that applies
 * the extended Reinhard curve of `include/orrery/viz/tone_map.hpp` and encodes
 * for the display. A galaxy is not a set of opaque objects, and what a picture
 * of one should show is the sum of light along each line of sight rather than
 * whichever particle happens to be nearest.
 *
 * The one difference from the native renderer is how a sprite becomes
 * geometry. OpenGL has `gl_PointSize` and WebGPU has no point size at all, so a
 * port would have had one backend drawing points and the other drawing quads,
 * which is precisely the sort of difference that ends with the two producing
 * different images. Both draw an instanced quad instead.
 */

import type { DepthRange } from './camera';

/** A colour in linear light, before tone mapping. */
export interface Colour {
  readonly red: number;
  readonly green: number;
  readonly blue: number;
}

/** Everything about how the picture looks that is not the camera. */
export interface RenderSettings {
  /** What the accumulated radiance is multiplied by before the curve. */
  readonly exposure: number;
  /** The exposed radiance that maps exactly to white. */
  readonly whitePoint: number;
  /** The diameter in pixels a particle would have one unit from the camera. */
  readonly pointSize: number;
  /** The smallest a particle is allowed to become, in pixels. */
  readonly minimumPointSize: number;
  /** The light one particle contributes at the centre of its sprite. */
  readonly brightness: number;
  /**
   * What the particles are drawn in.
   *
   * One colour rather than the two the native renderer ramps between. That ramp
   * is selected by a per-particle tint saying which galaxy a particle was
   * sampled into, and a trajectory records positions and no configuration, so
   * playing one back cannot tell the two galaxies apart. `orrery-view play`
   * has the same restriction for the same reason, and inventing a tint here
   * would be inventing the one thing the file does not say.
   */
  readonly colour: Colour;
}

/**
 * The defaults, which are the native viewer's.
 *
 * The point size and brightness are `RenderSettings` in
 * `include/orrery/viz/point_renderer.hpp`; the white point is `ToneMapping`'s;
 * the colour is the cool end of that renderer's ramp.
 */
export const DEFAULT_SETTINGS: RenderSettings = {
  exposure: 1.6,
  whitePoint: 4,
  pointSize: 40,
  minimumPointSize: 1.5,
  brightness: 1,
  colour: { red: 0.55, green: 0.7, blue: 1.0 },
};

/** The positions of one frame, as component arrays. */
export interface Positions {
  readonly count: number;
  readonly x: Float32Array;
  readonly y: Float32Array;
  readonly z: Float32Array;
}

export type Backend = 'webgpu' | 'webgl2';

export interface Renderer {
  readonly backend: Backend;
  /** What the device calls itself, for the line the plate carries. */
  readonly description: string;
  /** Which clip volume this backend's projection must target. */
  readonly depthRange: DepthRange;

  /** Change the size of the accumulation target, in device pixels. */
  resize(width: number, height: number): void;

  /**
   * Draw one frame: accumulate, then tone map onto the canvas.
   *
   * Allocates nothing. Every buffer it writes into was made when the renderer
   * was, and grown only when a run with more particles than the last arrives.
   */
  draw(
    positions: Positions,
    viewProjection: Float32Array,
    settings: RenderSettings,
  ): void;

  dispose(): void;
}

/** No backend would start, with the reason in the message. */
export class RendererError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'RendererError';
  }
}
