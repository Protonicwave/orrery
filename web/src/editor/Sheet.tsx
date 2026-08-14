import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { Design } from './design';
import type { Drawing, Point, Section, Shape, Weight } from './drawing';
import { primaryGalaxy, secondaryGalaxy, totalMass } from './elements';
import type { View } from './preview';
import styles from './Sheet.module.css';

/** How much of the view is left as margin around what is drawn. */
const MARGIN = 1.12;

/** The size of an arrowhead and of a centre mark, in pixels. */
const ARROWHEAD = 7;
const MARK = 4;

interface Placed {
  readonly view: View;
  /** A model point in pixels from the top left of the surface. */
  readonly at: (point: Point) => Point;
}

function place(width: number, height: number, extent: number): Placed {
  const scale = Math.min(width, height) / (2 * extent * MARGIN);
  const view: View = {
    scale,
    originX: width / 2,
    originY: height / 2,
    width,
    height,
  };
  return {
    view,
    at: (point) => ({
      x: view.originX + point.x * scale,
      y: view.originY - point.y * scale,
    }),
  };
}

function classOf(weight: Weight | undefined, dashed?: boolean): string {
  const parts = [styles[weight ?? 'body'] as string];
  if (dashed === true) parts.push(styles.dashed as string);
  return parts.join(' ');
}

/** The unit vector along a screen line, and the one perpendicular to it. */
function direction(from: Point, to: Point): { along: Point; across: Point } {
  const dx = to.x - from.x;
  const dy = to.y - from.y;
  const length = Math.hypot(dx, dy) || 1;
  const along = { x: dx / length, y: dy / length };
  return { along, across: { x: -along.y, y: along.x } };
}

function points(list: readonly Point[]): string {
  return list.map((point) => `${point.x.toFixed(2)},${point.y.toFixed(2)}`).join(' ');
}

function ShapeMark({ shape, at }: { shape: Shape; at: (point: Point) => Point }) {
  switch (shape.kind) {
    case 'path': {
      if (shape.points.length < 2) return null;
      const drawn = points(shape.points.map(at));
      const className = classOf(shape.weight, shape.dashed);
      return shape.closed === true ? (
        <polygon className={className} points={drawn} />
      ) : (
        <polyline className={className} points={drawn} />
      );
    }

    case 'ellipse': {
      // The unit circle carried through the same rotation the particles are, so
      // the ellipse is the projection of the disc rather than a fit to it.
      const centre = at(shape.centre);
      const zero = at({ x: 0, y: 0 });
      const u = at(shape.u);
      const v = at(shape.v);
      const matrix = [
        u.x - zero.x,
        u.y - zero.y,
        v.x - zero.x,
        v.y - zero.y,
        centre.x,
        centre.y,
      ]
        .map((value) => value.toFixed(4))
        .join(' ');
      return (
        <ellipse
          className={classOf(shape.weight, shape.dashed)}
          cx="0"
          cy="0"
          rx="1"
          ry="1"
          transform={`matrix(${matrix})`}
          vectorEffect="non-scaling-stroke"
        />
      );
    }

    case 'mark': {
      const centre = at(shape.at);
      return (
        <g className={classOf('furniture')}>
          <line x1={centre.x - MARK} y1={centre.y} x2={centre.x + MARK} y2={centre.y} />
          <line x1={centre.x} y1={centre.y - MARK} x2={centre.x} y2={centre.y + MARK} />
          {shape.label !== undefined && (
            <text className={styles.note} x={centre.x + 7} y={centre.y + 11}>
              {shape.label}
            </text>
          )}
        </g>
      );
    }

    case 'arrow': {
      const from = at(shape.from);
      const to = at(shape.to);
      const { along, across } = direction(from, to);
      const head = [
        to,
        {
          x: to.x - along.x * ARROWHEAD + across.x * ARROWHEAD * 0.45,
          y: to.y - along.y * ARROWHEAD + across.y * ARROWHEAD * 0.45,
        },
        {
          x: to.x - along.x * ARROWHEAD - across.x * ARROWHEAD * 0.45,
          y: to.y - along.y * ARROWHEAD - across.y * ARROWHEAD * 0.45,
        },
      ];
      return (
        <g className={classOf(shape.weight)}>
          <line x1={from.x} y1={from.y} x2={to.x} y2={to.y} />
          <polygon className={styles.head} points={points(head)} />
          {shape.label !== undefined && (
            <text
              className={styles.measure}
              x={to.x + across.x * 9}
              y={to.y + across.y * 9}
            >
              {shape.label}
            </text>
          )}
        </g>
      );
    }

    case 'dimension': {
      // A dimension line stands off from what it measures, with a witness line
      // at each end running back to the feature. That is what separates a
      // measurement from a line that happens to be there.
      const from = at(shape.from);
      const to = at(shape.to);
      const { across } = direction(from, to);
      const shift = { x: across.x * shape.offset, y: across.y * shape.offset };
      const a = { x: from.x + shift.x, y: from.y + shift.y };
      const b = { x: to.x + shift.x, y: to.y + shift.y };
      const middle = { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
      return (
        <g className={classOf('furniture')}>
          <line x1={from.x} y1={from.y} x2={a.x} y2={a.y} className={styles.witness} />
          <line x1={to.x} y1={to.y} x2={b.x} y2={b.y} className={styles.witness} />
          <line x1={a.x} y1={a.y} x2={b.x} y2={b.y} />
          <text
            className={styles.measure}
            x={middle.x}
            y={middle.y - 4}
            textAnchor="middle"
          >
            {shape.label}
          </text>
        </g>
      );
    }

    case 'angle': {
      // Screen angles run the other way from model angles, because the y axis
      // does.
      const centre = at(shape.at);
      const from = -shape.from;
      const to = -shape.to;
      const start = {
        x: centre.x + shape.radius * Math.cos(from),
        y: centre.y + shape.radius * Math.sin(from),
      };
      const end = {
        x: centre.x + shape.radius * Math.cos(to),
        y: centre.y + shape.radius * Math.sin(to),
      };
      const sweep = to > from ? 1 : 0;
      const middle = (from + to) / 2;
      return (
        <g className={classOf('furniture')}>
          <path
            d={`M ${start.x.toFixed(2)} ${start.y.toFixed(2)} A ${shape.radius} ${shape.radius} 0 0 ${sweep} ${end.x.toFixed(2)} ${end.y.toFixed(2)}`}
            fill="none"
          />
          <text
            className={styles.measure}
            x={centre.x + (shape.radius + 10) * Math.cos(middle)}
            y={centre.y + (shape.radius + 10) * Math.sin(middle) + 3}
          >
            {shape.label}
          </text>
        </g>
      );
    }

    case 'text': {
      const centre = at(shape.at);
      return (
        <text
          className={`${styles.note} ${classOf(shape.weight ?? 'furniture')}`}
          x={centre.x + (shape.dx ?? 0)}
          y={centre.y + (shape.dy ?? 0)}
          textAnchor={shape.anchor}
        >
          {shape.text}
        </text>
      );
    }
  }
}

/**
 * The section through a disc, on its own line of nodes.
 *
 * A separate small view rather than something in the plan, because an
 * inclination is a rotation out of the plane the plan is drawn in and cannot be
 * dimensioned there. Here the disc is a line, the plane it is measured from is
 * the horizontal, and the arc between them is the angle the configuration
 * states rather than a projection of it.
 */
function SectionDrawing({ section }: { section: Section }) {
  const size = 74;
  const half = size / 2;
  const arm = 26;
  // Wider than it is tall, so that the degree reading beside the arc has paper
  // to sit on rather than being cut off at the edge of the detail.
  const width = size + 30;
  const angle = section.inclination;
  const disc = { x: Math.cos(angle) * arm, y: -Math.sin(angle) * arm };
  const axis = { x: Math.sin(angle) * arm * 0.62, y: Math.cos(angle) * arm * 0.62 };
  const label = `${((angle * 180) / Math.PI).toFixed(1)}°`;

  return (
    <figure className={styles.section}>
      <svg
        viewBox={`0 0 ${width} ${size}`}
        width={width}
        height={size}
        aria-hidden="true"
      >
        <title>{`${section.title} inclination ${label}`}</title>
        <g className={styles.furniture}>
          <line x1={half - arm} y1={half} x2={half + arm} y2={half} />
        </g>
        <g className={styles.body}>
          <line
            x1={half - disc.x}
            y1={half - disc.y}
            x2={half + disc.x}
            y2={half + disc.y}
          />
        </g>
        <g className={styles.accent}>
          <line x1={half} y1={half} x2={half - axis.x} y2={half - axis.y} />
        </g>
        <path
          className={styles.furniture}
          fill="none"
          d={`M ${half + 14} ${half} A 14 14 0 0 ${angle > 0 ? 0 : 1} ${half + Math.cos(angle) * 14} ${half - Math.sin(angle) * 14}`}
        />
        <text className={styles.measure} x={half + 17} y={half - 6}>
          {label}
        </text>
      </svg>
      <figcaption>
        {section.title} · {section.retrograde ? 'retrograde' : 'prograde'}
      </figcaption>
    </figure>
  );
}

export interface SheetProps {
  design: Design;
  drawing: Drawing;
  /** Told where the surface put the model, so the preview can register with it. */
  onView: (view: View) => void;
  /** The canvas the preview paints into, placed under the drawing. */
  canvasRef: React.RefObject<HTMLCanvasElement | null>;
  /** Moving the secondary galaxy, in model units. Null where there is not one. */
  onMove: ((separation: number, impactParameter: number) => void) | null;
}

/**
 * The drawing surface: paper, the plan, the sections and the legend.
 *
 * The preview's canvas sits underneath the plan and is placed by the same
 * numbers, so a particle and a dimension line are at one scale. Nothing about
 * the canvas is React's: it is written to by `preview.ts` at whatever rate the
 * solver produces frames, and React is told only where to put it.
 */
export function Sheet({ design, drawing, onView, canvasRef, onMove }: SheetProps) {
  const containerRef = useRef<HTMLDivElement>(null);
  const [size, setSize] = useState({ width: 0, height: 0 });

  useEffect(() => {
    const container = containerRef.current;
    if (container === null) return;

    const observer = new ResizeObserver((entries) => {
      const entry = entries[0];
      if (entry === undefined) return;
      const { width, height } = entry.contentRect;
      setSize((current) =>
        current.width === width && current.height === height
          ? current
          : { width, height },
      );
    });
    observer.observe(container);
    return () => {
      observer.disconnect();
    };
  }, []);

  // Memoised so that the placement is one object per size rather than one per
  // render, which is what lets the effect below fire when the placement changes
  // and not when anything else does.
  const placed = useMemo(
    () => place(size.width, size.height, drawing.extent),
    [size.width, size.height, drawing.extent],
  );

  // The preview follows the drawing rather than the other way round, so the
  // view is published on every change to it and never read back.
  useEffect(() => {
    onView(placed.view);
  }, [onView, placed]);

  /**
   * Dragging the secondary galaxy.
   *
   * The two settings under the pointer are the separation and the impact
   * parameter, and the handle sets exactly those: a drag is a way of typing two
   * numbers rather than a mode of its own. The same two numbers have sliders in
   * the console, and this handle takes the arrow keys, so the drag is a second
   * way of reaching them rather than the only way.
   */
  const dragging = useRef(false);
  const model = useCallback(
    (clientX: number, clientY: number): { x: number; y: number } | null => {
      const container = containerRef.current;
      if (container === null || placed.view.scale === 0) return null;
      const box = container.getBoundingClientRect();
      return {
        x: (clientX - box.left - placed.view.originX) / placed.view.scale,
        y: -(clientY - box.top - placed.view.originY) / placed.view.scale,
      };
    },
    [placed.view],
  );

  // The secondary sits at the primary's share of the separation, because the
  // pair is placed in its own centre-of-mass frame. The handle is on the galaxy
  // rather than at the end of the separation vector for that reason.
  const share = primaryShare(design);
  const handle =
    onMove === null
      ? null
      : { x: design.separation * share, y: design.impactParameter * share };

  return (
    <div className={styles.surface} ref={containerRef}>
      {/* The preview is a picture of the drawing above it and carries no
          information the drawing does not, so it is left out of the
          accessibility tree and out of the tab order rather than described
          twice. */}
      <canvas
        className={styles.preview}
        ref={canvasRef}
        tabIndex={-1}
        aria-hidden="true"
      />

      <svg
        className={styles.plan}
        viewBox={`0 0 ${size.width} ${size.height}`}
        width={size.width}
        height={size.height}
        role="img"
        aria-label={drawing.description}
      >
        <title>{drawing.description}</title>

        {/* The axes of the model, which every measurement is taken from. */}
        <g className={styles.furniture}>
          <line
            x1={0}
            y1={placed.view.originY}
            x2={size.width}
            y2={placed.view.originY}
          />
          <line
            x1={placed.view.originX}
            y1={0}
            x2={placed.view.originX}
            y2={size.height}
          />
        </g>
        <text className={styles.axis} x={size.width - 10} y={placed.view.originY - 6}>
          x
        </text>
        <text className={styles.axis} x={placed.view.originX + 6} y={12}>
          y
        </text>

        {drawing.shapes.map((shape, index) => (
          // The shapes are a generated list with no identity of their own, and
          // they are replaced whole whenever the design changes.
          // biome-ignore lint/suspicious/noArrayIndexKey: a drawing has no keys
          <ShapeMark key={index} shape={shape} at={placed.at} />
        ))}
      </svg>

      {/* The one thing on the sheet that can be operated, on a surface of its
          own. The drawing above is described as a picture, and a picture may not
          contain something focusable: a reader who is told they are on an image
          and then finds a control inside it has been told the wrong thing. */}
      <svg
        className={styles.controls}
        viewBox={`0 0 ${size.width} ${size.height}`}
        width={size.width}
        height={size.height}
      >
        <title>What can be moved on the drawing</title>
        {handle !== null && onMove !== null && (
          // An SVG group rather than a button element, because a button cannot
          // be put inside a drawing and the thing being operated is a mark on
          // the drawing.
          // biome-ignore lint/a11y/useSemanticElements: SVG has no button
          <g
            className={styles.handle}
            role="button"
            tabIndex={0}
            aria-label={`Secondary galaxy: separation ${design.separation.toFixed(2)}, impact parameter ${design.impactParameter.toFixed(2)}. Drag or use the arrow keys.`}
            onPointerDown={(event) => {
              dragging.current = true;
              event.currentTarget.setPointerCapture(event.pointerId);
            }}
            onPointerMove={(event) => {
              if (!dragging.current) return;
              const at = model(event.clientX, event.clientY);
              if (at === null) return;
              onMove(at.x / share, at.y / share);
            }}
            onPointerUp={(event) => {
              dragging.current = false;
              event.currentTarget.releasePointerCapture(event.pointerId);
            }}
            onKeyDown={(event) => {
              const step = event.shiftKey ? 1 : 0.1;
              if (event.key === 'ArrowLeft')
                onMove(design.separation - step, design.impactParameter);
              else if (event.key === 'ArrowRight')
                onMove(design.separation + step, design.impactParameter);
              else if (event.key === 'ArrowUp')
                onMove(design.separation, design.impactParameter + step);
              else if (event.key === 'ArrowDown')
                onMove(design.separation, design.impactParameter - step);
              else return;
              event.preventDefault();
            }}
          >
            <circle
              cx={placed.at(handle).x}
              cy={placed.at(handle).y}
              r={13}
              fill="transparent"
            />
            <circle cx={placed.at(handle).x} cy={placed.at(handle).y} r={7} />
          </g>
        )}
      </svg>

      <div className={styles.sections}>
        {drawing.sections.map((section) => (
          <SectionDrawing key={section.title} section={section} />
        ))}
      </div>

      <div className={styles.legend}>
        <span>plan on x-y · G = 1</span>
        {drawing.velocityScale !== null && (
          <span>
            velocity drawn at {drawing.velocityScale.toFixed(1)} lengths a unit
          </span>
        )}
      </div>
    </div>
  );
}

/**
 * The primary's share of the pair's mass.
 *
 * From the realised masses rather than from the mass ratio, so that the handle
 * is on the galaxy the drawing drew rather than half a particle mass away from
 * it.
 */
function primaryShare(design: Design): number {
  const primary = totalMass(primaryGalaxy(design));
  const secondary = totalMass(secondaryGalaxy(design));
  const total = primary + secondary;
  return total > 0 ? primary / total : 0.5;
}
