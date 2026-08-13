/**
 * The camera, as three angles and a distance.
 *
 * A port of `include/orrery/viz/camera.hpp`, deliberately line for line: the
 * same four numbers, the same astronomical convention for the angles, the same
 * clamp away from the poles, the same multiplicative zoom and the same pan in
 * fractions of the frame height. The native viewer and this one are meant to
 * behave identically under the same drag, and the only way that holds is if
 * they are the same arithmetic rather than two cameras that felt similar to
 * whoever wrote them. `web/tests/render/camera.test.ts` checks the ported
 * arithmetic against the values `tests/viz/camera_test.cpp` asserts of the
 * original.
 *
 * ## Two depth conventions
 *
 * The one thing that is not a port. OpenGL's clip volume runs from minus one to
 * one in z and WebGPU's runs from zero to one, so a projection built for one is
 * clipped through the middle by the other. Rather than keep two cameras, the
 * projection takes the convention as an argument and each backend asks for its
 * own. Nothing else changes: the two matrices differ only in the third row, the
 * picture is identical, and there is no depth test to care about the values.
 *
 * ## No allocation
 *
 * Every method writes into an array the caller owns. The render loop asks for a
 * view-projection sixty times a second, and a camera that returned a fresh
 * matrix each time would be sixty allocations a second on the one path the
 * budget says allocates nothing.
 */

/** Which clip volume the projection should target. */
export type DepthRange = 'negative-one-to-one' | 'zero-to-one';

/**
 * The largest angle from the plane the camera will take, in radians.
 *
 * A hundredth of a radian short of the pole, as the native camera is. Exactly
 * at the pole the direction of view is parallel to the up vector and the view
 * transform has no defined orientation, which shows on screen as the image
 * spinning through a right angle as the camera crosses it.
 */
export const MAXIMUM_ELEVATION = 1.5607963;

/** The smallest distance the camera will hold. */
const MINIMUM_DISTANCE = 1e-4;

export class OrbitCamera {
  /** The point the camera looks at. Mutated in place, never replaced. */
  readonly target = { x: 0, y: 0, z: 0 };

  /** The distance from the target, which is what zooming changes. */
  distance = 30;

  /** The angle in the x-y plane, in radians, from the positive x axis. */
  azimuth = 0;

  /** The angle above the x-y plane, in radians. */
  elevation = 0.3;

  /** The whole vertical angle the frustum spans, in radians: forty degrees. */
  fieldOfView = 0.6981317;

  /** Near and far clipping distances, as fractions of the distance. */
  nearFraction = 0.01;
  farFraction = 10;

  /** Where the camera is. Rewritten on every read, and never handed out. */
  private readonly position = { x: 0, y: 0, z: 0 };

  private readonly viewMatrix = new Float32Array(16);
  private readonly projectionMatrix = new Float32Array(16);

  /** Where the camera is, from the target, the distance and the two angles. */
  eye(): { readonly x: number; readonly y: number; readonly z: number } {
    const horizontal = this.distance * Math.cos(this.elevation);
    this.position.x = this.target.x + horizontal * Math.cos(this.azimuth);
    this.position.y = this.target.y + horizontal * Math.sin(this.azimuth);
    this.position.z = this.target.z + this.distance * Math.sin(this.elevation);
    return this.position;
  }

  /**
   * The view transform, column-major, into `out`.
   *
   * Up is the world's z axis, fixed rather than carried in the camera's state:
   * the elevation is clamped away from the poles so the direction of view is
   * never parallel to it, and a fixed world up is what keeps the horizon level
   * through a drag that goes round in circles.
   */
  view(out: Float32Array): Float32Array {
    const eye = this.eye();

    // The basis vector that points backwards out of the screen, because the
    // camera looks down its own negative z.
    let bx = eye.x - this.target.x;
    let by = eye.y - this.target.y;
    let bz = eye.z - this.target.z;
    const backwardLength = Math.hypot(bx, by, bz) || 1;
    bx /= backwardLength;
    by /= backwardLength;
    bz /= backwardLength;

    // right = normalise(cross(up, backward)), with up the z axis, which makes
    // the cross product (-backward.y, backward.x, 0) before normalising.
    let rx = -by;
    let ry = bx;
    const rightLength = Math.hypot(rx, ry) || 1;
    rx /= rightLength;
    ry /= rightLength;

    // above = cross(backward, right), recomputed from the two axes already
    // fixed rather than taken from the caller, which leaves an orthonormal
    // basis whatever was passed in.
    const ax = -bz * ry;
    const ay = bz * rx;
    const az = bx * ry - by * rx;

    out[0] = rx;
    out[1] = ax;
    out[2] = bx;
    out[3] = 0;
    out[4] = ry;
    out[5] = ay;
    out[6] = by;
    out[7] = 0;
    out[8] = 0;
    out[9] = az;
    out[10] = bz;
    out[11] = 0;
    out[12] = -(rx * eye.x + ry * eye.y);
    out[13] = -(ax * eye.x + ay * eye.y + az * eye.z);
    out[14] = -(bx * eye.x + by * eye.y + bz * eye.z);
    out[15] = 1;
    return out;
  }

  /** The projection, column-major, into `out`. */
  projection(
    aspectRatio: number,
    depthRange: DepthRange,
    out: Float32Array,
  ): Float32Array {
    const focal = 1 / Math.tan(this.fieldOfView / 2);
    const near = this.distance * this.nearFraction;
    const far = this.distance * this.farFraction;

    out.fill(0);
    out[0] = focal / aspectRatio;
    out[5] = focal;
    out[11] = -1;

    if (depthRange === 'zero-to-one') {
      out[10] = far / (near - far);
      out[14] = (far * near) / (near - far);
    } else {
      out[10] = (far + near) / (near - far);
      out[14] = (2 * far * near) / (near - far);
    }
    return out;
  }

  /** The product of the two, which is the only matrix a renderer needs. */
  viewProjection(
    aspectRatio: number,
    depthRange: DepthRange,
    out: Float32Array,
  ): Float32Array {
    this.view(this.viewMatrix);
    this.projection(aspectRatio, depthRange, this.projectionMatrix);
    return multiply(this.projectionMatrix, this.viewMatrix, out);
  }

  /** Turn about the target, clamping the elevation away from the poles. */
  orbit(deltaAzimuth: number, deltaElevation: number): void {
    this.azimuth += deltaAzimuth;
    this.elevation = Math.min(
      MAXIMUM_ELEVATION,
      Math.max(-MAXIMUM_ELEVATION, this.elevation + deltaElevation),
    );
  }

  /**
   * Move towards or away from the target by a multiplicative factor.
   *
   * Multiplicative so that one notch of a wheel covers the same fraction of the
   * remaining distance wherever the camera is. A factor of zero or less is
   * ignored rather than clamped: there is no sensible value to clamp towards.
   */
  zoom(factor: number): void {
    if (!(factor > 0)) return;
    this.distance = Math.max(this.distance * factor, MINIMUM_DISTANCE);
  }

  /**
   * Slide the target across the plane of the screen.
   *
   * The arguments are fractions of the height of the frame at the distance of
   * the target, so a drag of a given number of pixels moves the image by that
   * many pixels whatever the camera's distance or field of view.
   */
  pan(right: number, up: number): void {
    const height = 2 * this.distance * Math.tan(this.fieldOfView / 2);
    const eye = this.eye();

    const fx = (this.target.x - eye.x) / this.distance;
    const fy = (this.target.y - eye.y) / this.distance;
    const fz = (this.target.z - eye.z) / this.distance;

    // across = cross(forward, up) with up the z axis, and screenUp =
    // cross(across, forward).
    let axisX = fy;
    let axisY = -fx;
    const acrossLength = Math.hypot(axisX, axisY);
    if (!(acrossLength > 0)) return;
    axisX /= acrossLength;
    axisY /= acrossLength;

    let ux = -axisY * fz;
    let uy = axisX * fz;
    let uz = axisX * fy - axisY * fx;
    const upLength = Math.hypot(ux, uy, uz);
    if (!(upLength > 0)) return;
    ux /= upLength;
    uy /= upLength;
    uz /= upLength;

    this.target.x += axisX * (right * height) + ux * (up * height);
    this.target.y += axisY * (right * height) + uy * (up * height);
    this.target.z += uz * (up * height);
  }
}

/**
 * `out = a * b`, column-major, where `out[column * 4 + row]`.
 *
 * Written out rather than looped. Sixteen dot products of four terms is a
 * hundred and twenty-eight multiplications either way, and the unrolled form
 * indexes constants rather than computing indices on a path that runs on every
 * frame.
 */
export function multiply(
  a: Float32Array,
  b: Float32Array,
  out: Float32Array,
): Float32Array {
  for (let column = 0; column < 4; column += 1) {
    const b0 = b[column * 4] as number;
    const b1 = b[column * 4 + 1] as number;
    const b2 = b[column * 4 + 2] as number;
    const b3 = b[column * 4 + 3] as number;
    for (let row = 0; row < 4; row += 1) {
      out[column * 4 + row] =
        (a[row] as number) * b0 +
        (a[4 + row] as number) * b1 +
        (a[8 + row] as number) * b2 +
        (a[12 + row] as number) * b3;
    }
  }
  return out;
}
