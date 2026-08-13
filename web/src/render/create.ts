/**
 * Choosing a backend.
 *
 * WebGPU first, WebGL2 second, and an honest sentence if neither will start.
 * The order is the plain one: WebGPU is the interface the platform is moving
 * to and WebGL2 is the one that runs everywhere, so a machine that has both
 * uses the newer and a machine that has one uses the one it has.
 *
 * A canvas holds one kind of context for its whole life, so a failed attempt
 * cannot be retried on the same element. This function therefore makes the
 * canvas rather than being handed one: a backend that will not start takes its
 * canvas with it and the next attempt begins on a fresh one.
 */

import { type Backend, type Renderer, RendererError } from './renderer';
import { createWebGl2Renderer } from './webgl2';
import { createWebGpuRenderer } from './webgpu';

export interface Started {
  readonly renderer: Renderer;
  readonly canvas: HTMLCanvasElement;
}

function attach(container: HTMLElement, className: string): HTMLCanvasElement {
  const canvas = document.createElement('canvas');
  canvas.className = className;
  container.append(canvas);
  return canvas;
}

function why(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/**
 * Start a renderer inside `container`, preferring `first`.
 *
 * `first` exists so that a test can ask for the backend it means to exercise
 * rather than the one the machine happens to prefer, which is what makes a
 * comparison between the two possible at all.
 */
export async function createRenderer(
  container: HTMLElement,
  className: string,
  first: Backend = 'webgpu',
): Promise<Started> {
  const reasons: string[] = [];

  const order: Backend[] =
    first === 'webgl2' ? ['webgl2', 'webgpu'] : ['webgpu', 'webgl2'];
  for (const backend of order) {
    const canvas = attach(container, className);
    try {
      const renderer =
        backend === 'webgpu'
          ? await createWebGpuRenderer(canvas)
          : createWebGl2Renderer(canvas);
      return { renderer, canvas };
    } catch (error) {
      canvas.remove();
      reasons.push(`${backend}: ${why(error)}`);
    }
  }

  throw new RendererError(
    `This browser will not draw the plate. ${reasons.join('. ')}.`,
  );
}
