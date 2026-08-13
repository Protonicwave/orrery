import { describe, expect, it } from 'vitest';
import { MAXIMUM_ELEVATION, multiply, OrbitCamera } from '../../src/render/camera';

/**
 * The ported camera, against what the original is asserted to do.
 *
 * The cases below are the ones in `tests/viz/camera_test.cpp`, with the same
 * numbers. That is the point of the file: the two viewers are meant to behave
 * identically under the same drag, and the only thing that can hold them
 * together is a test that fails when they drift.
 */

const TOLERANCE = 1e-5;

/** A point through a transform and the perspective divide, which is clip space. */
function project(
  matrix: Float32Array,
  x: number,
  y: number,
  z: number,
): { x: number; y: number; z: number; w: number } {
  const at = (row: number) =>
    (matrix[row] as number) * x +
    (matrix[4 + row] as number) * y +
    (matrix[8 + row] as number) * z +
    (matrix[12 + row] as number);

  const w = at(3);
  return { x: at(0) / w, y: at(1) / w, z: at(2) / w, w };
}

describe('the camera is where its angles put it', () => {
  it('is along the x axis when both angles are zero', () => {
    const camera = new OrbitCamera();
    camera.target.x = 1;
    camera.target.y = 2;
    camera.target.z = 3;
    camera.distance = 10;
    camera.azimuth = 0;
    camera.elevation = 0;

    const eye = camera.eye();
    expect(eye.x).toBeCloseTo(11, 5);
    expect(eye.y).toBeCloseTo(2, 5);
    expect(eye.z).toBeCloseTo(3, 5);
  });

  it('is overhead at the top of its range', () => {
    const camera = new OrbitCamera();
    camera.target.x = 1;
    camera.target.y = 2;
    camera.target.z = 3;
    camera.distance = 10;
    camera.elevation = Math.PI / 2;
    expect(camera.eye().z).toBeCloseTo(13, 5);
  });

  it('is always at the stated distance', () => {
    const camera = new OrbitCamera();
    camera.target.x = 1;
    camera.target.y = 2;
    camera.target.z = 3;
    camera.distance = 10;

    for (const azimuth of [0, 1, -2.5]) {
      for (const elevation of [-1, 0, 1.2]) {
        camera.azimuth = azimuth;
        camera.elevation = elevation;
        const eye = camera.eye();
        const away = Math.hypot(
          eye.x - camera.target.x,
          eye.y - camera.target.y,
          eye.z - camera.target.z,
        );
        expect(away).toBeCloseTo(10, 5);
      }
    }
  });
});

describe('the view transform', () => {
  it('puts the camera at the origin looking down negative z', () => {
    const camera = new OrbitCamera();
    camera.target.x = -4;
    camera.target.y = 7;
    camera.target.z = 1;
    camera.distance = 6;
    camera.azimuth = 0.9;
    camera.elevation = 0.4;

    const view = camera.view(new Float32Array(16));
    const eye = camera.eye();

    // The eye goes to the origin.
    const atEye = project(view, eye.x, eye.y, eye.z);
    expect(atEye.x * atEye.w).toBeCloseTo(0, 4);
    expect(atEye.y * atEye.w).toBeCloseTo(0, 4);
    expect(atEye.z * atEye.w).toBeCloseTo(0, 4);

    // The target goes onto the negative z axis, at the distance it is away.
    const atTarget = project(view, camera.target.x, camera.target.y, camera.target.z);
    expect(atTarget.x * atTarget.w).toBeCloseTo(0, 4);
    expect(atTarget.y * atTarget.w).toBeCloseTo(0, 4);
    expect(atTarget.z * atTarget.w).toBeCloseTo(-6, 4);
  });

  it('keeps the horizon level whatever the azimuth', () => {
    const camera = new OrbitCamera();
    camera.distance = 10;
    const view = new Float32Array(16);

    // The world's z axis is up on the screen, which is what a fixed world up
    // buys and what a camera carrying its own would slowly lose.
    for (const azimuth of [0, 1.3, -2.2, 5]) {
      camera.azimuth = azimuth;
      camera.elevation = 0.2;
      camera.view(view);
      // The screen's right, which is the first row of the rotation, has no
      // component along the world's z.
      expect(view[8] as number).toBeCloseTo(0, 6);
    }
  });
});

describe('the projection', () => {
  it('maps the near and far planes onto the clip volume it was asked for', () => {
    const camera = new OrbitCamera();
    camera.distance = 10;
    const near = camera.distance * camera.nearFraction;
    const far = camera.distance * camera.farFraction;

    const opengl = camera.projection(1.5, 'negative-one-to-one', new Float32Array(16));
    expect(project(opengl, 0, 0, -near).z).toBeCloseTo(-1, 4);
    expect(project(opengl, 0, 0, -far).z).toBeCloseTo(1, 4);

    // WebGPU's clip volume runs from zero rather than from minus one, and a
    // projection built for the other one would be clipped through the middle.
    const webgpu = camera.projection(1.5, 'zero-to-one', new Float32Array(16));
    expect(project(webgpu, 0, 0, -near).z).toBeCloseTo(0, 4);
    expect(project(webgpu, 0, 0, -far).z).toBeCloseTo(1, 4);

    // The two differ in depth and in nothing else, which is what makes the two
    // backends draw the same picture.
    for (const index of [0, 5, 11]) {
      expect(webgpu[index] as number).toBeCloseTo(opengl[index] as number, 6);
    }
  });

  it('narrows with the aspect ratio rather than widening', () => {
    const camera = new OrbitCamera();
    const wide = camera.projection(2, 'zero-to-one', new Float32Array(16));
    const square = camera.projection(1, 'zero-to-one', new Float32Array(16));

    // A wider frame shows more of the world across, so a point at a fixed angle
    // from the axis lands nearer the middle of it.
    expect(Math.abs(wide[0] as number)).toBeLessThan(Math.abs(square[0] as number));
    expect(wide[5] as number).toBeCloseTo(square[5] as number, 6);
  });
});

describe('the controls', () => {
  it('clamps the elevation away from the poles', () => {
    const camera = new OrbitCamera();
    camera.orbit(0, 10);
    expect(camera.elevation).toBeCloseTo(MAXIMUM_ELEVATION, 6);
    camera.orbit(0, -20);
    expect(camera.elevation).toBeCloseTo(-MAXIMUM_ELEVATION, 6);
  });

  it('zooms by a factor rather than by a step', () => {
    const camera = new OrbitCamera();
    camera.distance = 30;
    camera.zoom(0.5);
    expect(camera.distance).toBeCloseTo(15, 6);
    camera.zoom(0.5);
    expect(camera.distance).toBeCloseTo(7.5, 6);

    // A factor of zero or less is not a distance, and is ignored rather than
    // clamped towards a value that has no meaning.
    camera.zoom(0);
    camera.zoom(-2);
    expect(camera.distance).toBeCloseTo(7.5, 6);
  });

  it('pans across the plane of the screen and not through it', () => {
    const camera = new OrbitCamera();
    camera.distance = 10;
    camera.azimuth = 0;
    camera.elevation = 0;

    // Looking down the x axis from positive x, so the screen's right is the
    // negative y direction and up is the world's z.
    camera.pan(1, 0);
    expect(camera.target.x).toBeCloseTo(0, 5);
    expect(camera.target.z).toBeCloseTo(0, 5);
    expect(camera.target.y).not.toBeCloseTo(0, 3);

    const across = camera.target.y;
    camera.pan(0, 1);
    expect(camera.target.y).toBeCloseTo(across, 5);
    expect(camera.target.z).toBeCloseTo(2 * 10 * Math.tan(camera.fieldOfView / 2), 4);
  });
});

describe('multiplying two transforms', () => {
  it('is the product in the order it is written', () => {
    // A scale and a translation, in column-major order, whose product is easy
    // to state: scaling then translating is not translating then scaling.
    const scale = new Float32Array([2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1]);
    const move = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1]);

    const out = multiply(scale, move, new Float32Array(16));
    expect([...out.slice(12, 15)]).toEqual([10, 18, 28]);
    expect(out[0]).toBe(2);

    const other = multiply(move, scale, new Float32Array(16));
    expect([...other.slice(12, 15)]).toEqual([5, 6, 7]);
  });

  it('leaves a matrix alone when the other is the identity', () => {
    const identity = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
    const camera = new OrbitCamera();
    const view = camera.view(new Float32Array(16));
    const out = multiply(identity, view, new Float32Array(16));
    for (let index = 0; index < 16; index += 1) {
      expect(Math.abs((out[index] as number) - (view[index] as number))).toBeLessThan(
        TOLERANCE,
      );
    }
  });
});
