/**
 * How a column of a diagnostics file is drawn.
 *
 * A sparkline: the line, and nothing else. No gridlines, no legend, no axis
 * furniture, no tick labels. What a reader needs in order to read one is the
 * shape, the current value direct-labelled above it and the ends of the range
 * beneath it, and every mark beyond those three is ink spent on the plot rather
 * than on the measurement.
 *
 * The one mark that is not the line is a rule at zero, drawn only when zero is
 * inside the range being plotted. A conserved quantity is a claim about a
 * distance from zero, so where zero sits is the whole of what the plot says; a
 * column that never approaches it has nothing to gain from a rule it cannot
 * reach.
 *
 * Colours are read from the custom properties on the element rather than
 * written here. A canvas cannot inherit a colour, so the alternative is a
 * second definition of the ink ramp in a file that is not the token file, and
 * the design system's one rule is that there is no second definition.
 */

/** What a plot is scaled against. */
export interface Range {
  readonly low: number;
  readonly high: number;
}

/** How much of the height is left above and below the line. */
const PADDING = 0.14;

/** Retina and no further. A sparkline is 26 pixels tall. */
const MAXIMUM_RATIO = 2;

function property(element: Element, name: string): string {
  return getComputedStyle(element).getPropertyValue(name).trim();
}

/**
 * The range a column is drawn against, padded so the line never touches the
 * edge of its box.
 *
 * A column that does not change at all is given a range around its own value
 * rather than a range of zero height, so that a conserved quantity draws a
 * straight line through the middle instead of dividing by nothing.
 */
export function plotRange(low: number, high: number): Range {
  if (!(Number.isFinite(low) && Number.isFinite(high))) return { low: 0, high: 1 };
  if (high === low) {
    const size = Math.abs(low) || 1;
    return { low: low - size * 0.5, high: high + size * 0.5 };
  }
  const margin = (high - low) * PADDING;
  return { low: low - margin, high: high + margin };
}

/**
 * Draw a column into a canvas, sized to the box the layout gave it.
 *
 * Called when the run changes and when the element is resized, and not on a
 * frame: the line does not move while a run is playing. What moves is the
 * cursor, which is an element over the top of this one so that following it
 * costs one style write rather than a redraw of four plots.
 */
export function drawSeries(
  canvas: HTMLCanvasElement,
  values: Float64Array,
  range: Range,
): void {
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  if (width === 0 || height === 0) return;

  const ratio = Math.min(window.devicePixelRatio || 1, MAXIMUM_RATIO);
  canvas.width = Math.round(width * ratio);
  canvas.height = Math.round(height * ratio);

  const context = canvas.getContext('2d');
  if (context === null) return;
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);

  const span = range.high - range.low;
  if (span <= 0) return;
  const y = (value: number): number => height - ((value - range.low) / span) * height;

  if (range.low < 0 && range.high > 0) {
    // Half a pixel, so a one pixel rule lands on a pixel rather than across two.
    const zero = Math.round(y(0)) + 0.5;
    context.strokeStyle = property(canvas, '--ink-4');
    context.lineWidth = 1;
    context.beginPath();
    context.moveTo(0, zero);
    context.lineTo(width, zero);
    context.stroke();
  }

  if (values.length === 0) return;

  context.strokeStyle = property(canvas, '--ink-2');
  context.lineWidth = 1;
  context.beginPath();
  if (values.length === 1) {
    const only = y(values[0] as number);
    context.moveTo(0, only);
    context.lineTo(width, only);
  } else {
    const last = values.length - 1;
    for (let index = 0; index <= last; index += 1) {
      const px = (index / last) * width;
      const py = y(values[index] as number);
      if (index === 0) context.moveTo(px, py);
      else context.lineTo(px, py);
    }
  }
  context.stroke();
}
