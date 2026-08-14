import { describe, expect, it } from 'vitest';
import { PRESETS, preset } from '../../src/editor/design';
import { drawingOf, type Point, type Shape } from '../../src/editor/drawing';
import { primaryGalaxy, secondaryGalaxy, totalMass } from '../../src/editor/elements';

/**
 * The drawing, as geometry rather than as a picture.
 *
 * What is checked here is what the drawing claims: that a mark is where the
 * sampler will put a galaxy, that a dimension measures the setting it is labelled
 * with, and that the ellipse is the projection of a tilted circle rather than a
 * circle drawn smaller. None of that needs a surface to be drawn on, which is why
 * `drawing.ts` returns shapes and draws nothing.
 */

function marks(shapes: readonly Shape[]): Point[] {
  return shapes.filter((shape) => shape.kind === 'mark').map((shape) => shape.at);
}

function labels(shapes: readonly Shape[]): string[] {
  return shapes.flatMap((shape) =>
    shape.kind === 'dimension' || shape.kind === 'angle'
      ? [shape.label]
      : shape.kind === 'text'
        ? [shape.text]
        : [],
  );
}

describe('every design', () => {
  it.each(PRESETS.map((entry) => entry.id))('is drawable: %s', (kind) => {
    const drawing = drawingOf(preset(kind));

    expect(drawing.shapes.length).toBeGreaterThan(3);
    expect(Number.isFinite(drawing.extent)).toBe(true);
    expect(drawing.extent).toBeGreaterThan(0);
    expect(drawing.description).not.toBe('');

    // Nothing may be drawn at a coordinate that cannot be turned into a
    // position on the paper.
    for (const shape of drawing.shapes) {
      const points =
        shape.kind === 'path'
          ? shape.points
          : shape.kind === 'ellipse'
            ? [shape.centre, shape.u, shape.v]
            : shape.kind === 'arrow'
              ? [shape.from, shape.to]
              : shape.kind === 'dimension'
                ? [shape.from, shape.to]
                : [shape.at];
      for (const point of points) {
        expect(Number.isFinite(point.x)).toBe(true);
        expect(Number.isFinite(point.y)).toBe(true);
      }
    }
  });
});

describe('the collision drawing', () => {
  const design = preset('galaxy-collision');
  const drawing = drawingOf(design);

  it('puts the two galaxies where the placement puts them', () => {
    const primary = totalMass(primaryGalaxy(design));
    const secondary = totalMass(secondaryGalaxy(design));
    const total = primary + secondary;

    // The pair is placed in its own centre-of-mass frame, so each galaxy sits on
    // its own side of the origin in inverse proportion to its mass.
    const centres = marks(drawing.shapes);
    expect(centres).toContainEqual({ x: 0, y: 0 });

    const first = centres.find((point) => point.x < 0);
    const second = centres.find((point) => point.x > 0);
    expect(first?.x).toBeCloseTo((-design.separation * secondary) / total, 12);
    expect(first?.y).toBeCloseTo((-design.impactParameter * secondary) / total, 12);
    expect(second?.x).toBeCloseTo((design.separation * primary) / total, 12);
    expect(second?.y).toBeCloseTo((design.impactParameter * primary) / total, 12);

    // And the mass-weighted mean of the two is the origin, which is the
    // statement the drawing's centre mark makes.
    const weighted = ((first?.x ?? 0) * primary + (second?.x ?? 0) * secondary) / total;
    expect(weighted).toBeCloseTo(0, 12);
  });

  it('dimensions the settings rather than lengths derived from them', () => {
    expect(labels(drawing.shapes)).toContain('separation 20.0');
    expect(labels(drawing.shapes)).toContain('b 2.00');
  });

  it('sections each disc at the angle the configuration states', () => {
    expect(drawing.sections).toHaveLength(2);
    expect(drawing.sections[0]?.inclination).toBe(design.inclination);
    expect(drawing.sections[1]?.inclination).toBe(design.secondaryInclination);
    expect(drawing.sections.every((section) => !section.retrograde)).toBe(true);

    // A disc turned past a right angle is retrograde, and the section says so
    // in a word rather than leaving it to the drawing.
    const over = drawingOf({ ...design, inclination: 2.5 });
    expect(over.sections[0]?.retrograde).toBe(true);
  });

  it('draws a velocity at a scale it states', () => {
    expect(drawing.velocityScale).not.toBeNull();
    expect(drawing.velocityScale ?? 0).toBeGreaterThan(0);

    // Nothing moves in a pair released at rest, so there is no scale to state
    // and no arrow to draw.
    const still = drawingOf({ ...design, approachSpeed: 0 });
    expect(still.velocityScale).toBeNull();
    expect(still.shapes.some((shape) => shape.kind === 'arrow')).toBe(false);
  });

  it('describes itself in a sentence that says what the encounter is', () => {
    expect(drawing.description).toContain('bound');
    expect(drawingOf({ ...design, approachSpeed: 1.4 }).description).toContain(
      'unbound',
    );
  });
});

describe('a disc seen in plan', () => {
  it('is a circle when it is face on and an ellipse when it is not', () => {
    const design = preset('disc-galaxy');
    const galaxy = { ...design, inclination: 0, positionAngle: 0 };
    const flat = drawingOf(galaxy).shapes.find((shape) => shape.kind === 'ellipse');
    expect(flat?.kind).toBe('ellipse');
    if (flat?.kind !== 'ellipse') return;

    expect(Math.hypot(flat.u.x, flat.u.y)).toBeCloseTo(
      Math.hypot(flat.v.x, flat.v.y),
      12,
    );

    // Tilted about the line of nodes, the axis across it is foreshortened by
    // the cosine of the inclination and the one along it is not. That is what
    // makes the ellipse the projection of the disc rather than a smaller circle.
    const tilt = 0.6;
    const tilted = drawingOf({
      ...design,
      inclination: tilt,
      positionAngle: 0,
    }).shapes.find((shape) => shape.kind === 'ellipse');
    if (tilted?.kind !== 'ellipse') throw new Error('the disc has no outline');
    expect(Math.hypot(tilted.u.x, tilted.u.y)).toBeCloseTo(
      Math.hypot(flat.u.x, flat.u.y),
      12,
    );
    expect(Math.hypot(tilted.v.x, tilted.v.y)).toBeCloseTo(
      Math.hypot(flat.v.x, flat.v.y) * Math.cos(tilt),
      12,
    );
  });
});
