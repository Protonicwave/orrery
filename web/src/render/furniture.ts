/**
 * The plate's functional furniture.
 *
 * A scale bar and an axis gnomon, both of which say something about the camera
 * and therefore have to be redrawn as the camera moves. Everything else the
 * plate carries, the run identifier, the seed, the exposure, the device, is a
 * value that changes at the speed a person changes it, so React sets it and
 * this file leaves it alone. Two writers on one element is a class of bug worth
 * not having.
 *
 * Furniture on an instrument is not decoration. A scale bar means a length on
 * screen can be read as a length in the model, and a gnomon means an
 * orientation can be recovered after a drag has lost it. Both are drawn at the
 * weight furniture is drawn at and neither is drawn where the picture is.
 *
 * Nothing here allocates unless something has changed. The loop calls `update`
 * sixty times a second and a camera that has not moved produces no writes at
 * all, which is what keeps a still plate from being a source of layout work.
 */

import { decimal } from '../format/number';
import type { PlateReadout } from './instrument';

/** The DOM the plate lends this module. */
export interface FurnitureElements {
  /** The rule whose width is the scale. */
  readonly scaleBar: HTMLElement;
  /** The length that rule stands for, in model units. */
  readonly scaleLabel: HTMLElement;
  /** One line per axis, from the gnomon's origin. */
  readonly axes: readonly [SVGLineElement, SVGLineElement, SVGLineElement];
  /** The letter at the end of each. */
  readonly axisLabels: readonly [SVGTextElement, SVGTextElement, SVGTextElement];
}

export interface PlateFurniture {
  update(readout: PlateReadout): void;
}

/** The lengths a scale bar is allowed to stand for, times a power of ten. */
const STEPS = [1, 2, 5] as const;

/** How long the bar may be on screen, in CSS pixels. */
const SHORTEST = 60;
const LONGEST = 170;

/** The gnomon's arm length and where its labels sit, in its own units. */
const ARM = 15;
const LABEL = 22;

/**
 * The longest of 1, 2 or 5 times a power of ten that fits in the bar's range.
 *
 * Returns zero when the view is at a scale where no such length fits, which
 * happens only if the camera has been zoomed several orders of magnitude past
 * anything the run contains. The bar is then hidden rather than lying.
 */
export function scaleLength(pixelsPerUnit: number): number {
  if (!(pixelsPerUnit > 0) || !Number.isFinite(pixelsPerUnit)) return 0;

  const exponent = Math.floor(Math.log10(LONGEST / pixelsPerUnit));
  for (let power = exponent + 1; power >= exponent - 1; power -= 1) {
    for (let index = STEPS.length - 1; index >= 0; index -= 1) {
      const length = (STEPS[index] as number) * 10 ** power;
      const pixels = length * pixelsPerUnit;
      if (pixels <= LONGEST && pixels >= SHORTEST) return length;
    }
  }
  return 0;
}

/** How a length in model units is written on the bar. */
export function scaleText(length: number): string {
  if (length === 0) return '';
  const digits = length >= 1 ? 0 : Math.min(6, Math.ceil(-Math.log10(length)));
  return decimal(length, digits);
}

export function createFurniture(elements: FurnitureElements): PlateFurniture {
  let lastLength = Number.NaN;
  let lastPixels = Number.NaN;
  let lastAzimuth = Number.NaN;
  let lastElevation = Number.NaN;

  return {
    update(readout) {
      const { pixelsPerUnit, camera } = readout;

      // The readout measures in device pixels and the bar is laid out in CSS
      // ones, so the ratio comes back out here rather than being carried
      // through the render loop, where it would be a second unit to keep track
      // of on the one path that must not have any.
      const ratio = window.devicePixelRatio || 1;
      const perCssPixel = pixelsPerUnit / ratio;
      const length = scaleLength(perCssPixel);
      const pixels = Math.round(length * perCssPixel);

      if (length !== lastLength) {
        lastLength = length;
        elements.scaleLabel.textContent = scaleText(length);
        elements.scaleBar.hidden = length === 0;
        elements.scaleLabel.hidden = length === 0;
      }
      if (pixels !== lastPixels) {
        lastPixels = pixels;
        elements.scaleBar.style.width = `${pixels}px`;
      }

      if (camera.azimuth === lastAzimuth && camera.elevation === lastElevation) {
        return;
      }
      lastAzimuth = camera.azimuth;
      lastElevation = camera.elevation;

      // Each world axis, rotated into the camera's frame. The view matrix is
      // column-major, so the rotation of the unit x axis is its first column,
      // and the screen's second coordinate runs down while the camera's runs
      // up. The gnomon is small enough that the perspective divide over it
      // would be invisible, so this is the rotation alone.
      const view = readout.viewMatrix;
      for (let axis = 0; axis < 3; axis += 1) {
        const right = view[axis * 4] as number;
        const up = view[axis * 4 + 1] as number;
        const line = elements.axes[axis] as SVGLineElement;
        const label = elements.axisLabels[axis] as SVGTextElement;

        line.setAttribute('x2', (right * ARM).toFixed(2));
        line.setAttribute('y2', (-up * ARM).toFixed(2));
        label.setAttribute('x', (right * LABEL).toFixed(2));
        label.setAttribute('y', (-up * LABEL).toFixed(2));
      }
    },
  };
}
