/**
 * The WebGL2 backend.
 *
 * The fallback, and the one that runs almost everywhere. It is a close port of
 * `src/viz/point_renderer.cpp`: the same two passes, the same Gaussian sprite,
 * the same additive blend into a half-float target, the same extended Reinhard
 * curve. Read the two side by side and the shaders are the same shaders.
 *
 * Two deliberate differences, both forced and both stated where they happen: a
 * sprite is an instanced quad rather than a `gl_PointSize` point, because
 * WebGPU has no point size and the two backends have to agree; and the sRGB
 * transfer function is applied in the shader rather than by the hardware,
 * because a WebGL2 default framebuffer is not an sRGB one and doing it by hand
 * is what makes this backend and the WebGPU one write identical bytes.
 */

import type { DepthRange } from './camera';
import {
  type Positions,
  type Renderer,
  RendererError,
  type RenderSettings,
} from './renderer';

const POINT_VERTEX = `#version 300 es
layout(location = 0) in vec2 corner;
layout(location = 1) in float position_x;
layout(location = 2) in float position_y;
layout(location = 3) in float position_z;

uniform mat4 view_projection;
uniform vec2 viewport;
uniform float point_size;
uniform float minimum_point_size;

out vec2 sprite;

void main() {
    vec4 clip = view_projection * vec4(position_x, position_y, position_z, 1.0);

    // The guard on w keeps a particle level with or behind the eye from
    // producing a negative or enormous sprite. It is clipped away regardless,
    // but an invalid size is undefined rather than merely invisible.
    float diameter = max(point_size / max(clip.w, 0.001), minimum_point_size);

    // A diameter of d pixels is d / viewport in normalised device coordinates,
    // and the offset is multiplied by w so that it survives the perspective
    // divide the rasteriser is about to perform.
    vec2 offset = corner * (diameter / viewport) * clip.w;

    gl_Position = vec4(clip.xy + offset, clip.z, clip.w);
    sprite = corner;
}
`;

const POINT_FRAGMENT = `#version 300 es
precision highp float;

in vec2 sprite;

uniform vec3 colour;
uniform float brightness;

out vec4 fragment;

void main() {
    float radius_squared = dot(sprite, sprite);
    if (radius_squared > 1.0) {
        discard;
    }

    // A Gaussian with its value at the rim subtracted, which brings the edge to
    // exactly zero. Without the subtraction every sprite ends in a step of about
    // two per cent of its peak, and a hundred thousand such steps are visible as
    // texture in the faint parts of the image.
    const float sharpness = 4.0;
    float falloff = exp(-sharpness * radius_squared) - exp(-sharpness);

    fragment = vec4(colour * brightness * falloff, 1.0);
}
`;

const POST_VERTEX = `#version 300 es
out vec2 texture_coordinate;

void main() {
    texture_coordinate = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4((texture_coordinate * 2.0) - 1.0, 0.0, 1.0);
}
`;

/**
 * The tone mapping pass.
 *
 * The curve is `orrery_tone_curve` from `include/orrery/viz/tone_map.hpp`,
 * transliterated. The sRGB encoding below it is the IEC 61966-2-1 transfer
 * function, the same constants `encode_srgb` uses in that file's
 * implementation, applied here because nothing else is going to apply it.
 */
const POST_FRAGMENT = `#version 300 es
precision highp float;

in vec2 texture_coordinate;

uniform sampler2D scene;
uniform float exposure;
uniform float white_point;

out vec4 fragment;

vec3 orrery_tone_curve(vec3 radiance, float exposure, float white_point) {
    vec3 exposed = max(radiance * exposure, vec3(0.0));
    vec3 mapped = exposed * (1.0 + exposed / (white_point * white_point)) / (1.0 + exposed);
    return clamp(mapped, 0.0, 1.0);
}

vec3 encode_srgb(vec3 linear) {
    vec3 low = linear * 12.92;
    vec3 high = (1.055 * pow(linear, vec3(1.0 / 2.4))) - 0.055;
    return mix(high, low, step(linear, vec3(0.0031308)));
}

void main() {
    vec3 radiance = texture(scene, texture_coordinate).rgb;
    fragment = vec4(encode_srgb(orrery_tone_curve(radiance, exposure, white_point)), 1.0);
}
`;

/** The unit quad every sprite is an instance of, as a triangle strip. */
const CORNERS = new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]);

function compile(
  gl: WebGL2RenderingContext,
  stage: number,
  source: string,
): WebGLShader {
  const shader = gl.createShader(stage);
  if (shader === null) throw new RendererError('this driver would not make a shader');

  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(shader) ?? 'the driver gave no reason';
    gl.deleteShader(shader);
    throw new RendererError(`a shader did not compile: ${log}`);
  }
  return shader;
}

function link(
  gl: WebGL2RenderingContext,
  vertex: string,
  fragment: string,
): WebGLProgram {
  const vertexShader = compile(gl, gl.VERTEX_SHADER, vertex);
  const fragmentShader = compile(gl, gl.FRAGMENT_SHADER, fragment);
  const program = gl.createProgram();
  if (program === null) {
    throw new RendererError('this driver would not make a shader program');
  }

  gl.attachShader(program, vertexShader);
  gl.attachShader(program, fragmentShader);
  gl.linkProgram(program);

  // Detached and deleted immediately: a shader attached to a program is kept
  // alive by it, so the program owns them for the rest of its life and there is
  // no separate bookkeeping.
  gl.deleteShader(vertexShader);
  gl.deleteShader(fragmentShader);

  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    const log = gl.getProgramInfoLog(program) ?? 'the driver gave no reason';
    gl.deleteProgram(program);
    throw new RendererError(`a shader program did not link: ${log}`);
  }
  return program;
}

class WebGl2Renderer implements Renderer {
  readonly backend = 'webgl2' as const;
  readonly depthRange: DepthRange = 'negative-one-to-one';
  readonly description: string;

  private readonly gl: WebGL2RenderingContext;
  private readonly pointProgram: WebGLProgram;
  private readonly postProgram: WebGLProgram;
  private readonly particleArray: WebGLVertexArrayObject;
  private readonly postArray: WebGLVertexArrayObject;
  private readonly cornerBuffer: WebGLBuffer;
  private readonly positionBuffers: [WebGLBuffer, WebGLBuffer, WebGLBuffer];
  private readonly framebuffer: WebGLFramebuffer;
  private readonly colourTexture: WebGLTexture;

  private readonly viewProjectionAt: WebGLUniformLocation | null;
  private readonly viewportAt: WebGLUniformLocation | null;
  private readonly pointSizeAt: WebGLUniformLocation | null;
  private readonly minimumPointSizeAt: WebGLUniformLocation | null;
  private readonly colourAt: WebGLUniformLocation | null;
  private readonly brightnessAt: WebGLUniformLocation | null;
  private readonly sceneAt: WebGLUniformLocation | null;
  private readonly exposureAt: WebGLUniformLocation | null;
  private readonly whitePointAt: WebGLUniformLocation | null;

  private width = 1;
  private height = 1;
  /** Particles the position buffers currently have room for. */
  private capacity = 0;

  constructor(gl: WebGL2RenderingContext) {
    this.gl = gl;

    // A half-float target is not optional: it is what lets the sum along a line
    // of sight exceed one, which is the whole reason the picture is accumulated
    // rather than composited. Without it the middle of a galaxy would saturate
    // after a few dozen particles and carry no information at all.
    if (
      gl.getExtension('EXT_color_buffer_half_float') === null &&
      gl.getExtension('EXT_color_buffer_float') === null
    ) {
      throw new RendererError(
        'this driver will not render into a floating-point target, which the accumulation pass needs',
      );
    }

    this.pointProgram = link(gl, POINT_VERTEX, POINT_FRAGMENT);
    this.postProgram = link(gl, POST_VERTEX, POST_FRAGMENT);

    this.viewProjectionAt = gl.getUniformLocation(this.pointProgram, 'view_projection');
    this.viewportAt = gl.getUniformLocation(this.pointProgram, 'viewport');
    this.pointSizeAt = gl.getUniformLocation(this.pointProgram, 'point_size');
    this.minimumPointSizeAt = gl.getUniformLocation(
      this.pointProgram,
      'minimum_point_size',
    );
    this.colourAt = gl.getUniformLocation(this.pointProgram, 'colour');
    this.brightnessAt = gl.getUniformLocation(this.pointProgram, 'brightness');
    this.sceneAt = gl.getUniformLocation(this.postProgram, 'scene');
    this.exposureAt = gl.getUniformLocation(this.postProgram, 'exposure');
    this.whitePointAt = gl.getUniformLocation(this.postProgram, 'white_point');

    const array = gl.createVertexArray();
    const post = gl.createVertexArray();
    const corners = gl.createBuffer();
    const framebuffer = gl.createFramebuffer();
    const texture = gl.createTexture();
    const buffers = [gl.createBuffer(), gl.createBuffer(), gl.createBuffer()];
    if (
      array === null ||
      post === null ||
      corners === null ||
      framebuffer === null ||
      texture === null ||
      buffers.some((buffer) => buffer === null)
    ) {
      throw new RendererError('this driver would not allocate the renderer objects');
    }

    this.particleArray = array;
    this.postArray = post;
    this.cornerBuffer = corners;
    this.framebuffer = framebuffer;
    this.colourTexture = texture;
    this.positionBuffers = buffers as [WebGLBuffer, WebGLBuffer, WebGLBuffer];

    // The attribute layout is recorded once. The corner is per vertex and the
    // three components are per instance, which is what a component array is:
    // one float per particle from a buffer of its own, no interleaving and no
    // gather.
    gl.bindVertexArray(this.particleArray);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.cornerBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, CORNERS, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 8, 0);
    for (let index = 0; index < 3; index += 1) {
      gl.bindBuffer(gl.ARRAY_BUFFER, this.positionBuffers[index] as WebGLBuffer);
      gl.enableVertexAttribArray(index + 1);
      gl.vertexAttribPointer(index + 1, 1, gl.FLOAT, false, 4, 0);
      gl.vertexAttribDivisor(index + 1, 1);
    }
    gl.bindVertexArray(null);

    gl.bindTexture(gl.TEXTURE_2D, this.colourTexture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    this.allocateTarget();

    const debug = gl.getExtension('WEBGL_debug_renderer_info');
    const device =
      debug === null
        ? gl.getParameter(gl.RENDERER)
        : gl.getParameter(debug.UNMASKED_RENDERER_WEBGL);
    this.description = `WebGL2, ${String(device)}`;
  }

  private allocateTarget(): void {
    const gl = this.gl;
    gl.bindTexture(gl.TEXTURE_2D, this.colourTexture);
    gl.texImage2D(
      gl.TEXTURE_2D,
      0,
      gl.RGBA16F,
      this.width,
      this.height,
      0,
      gl.RGBA,
      gl.HALF_FLOAT,
      null,
    );

    gl.bindFramebuffer(gl.FRAMEBUFFER, this.framebuffer);
    gl.framebufferTexture2D(
      gl.FRAMEBUFFER,
      gl.COLOR_ATTACHMENT0,
      gl.TEXTURE_2D,
      this.colourTexture,
      0,
    );
    const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    if (status !== gl.FRAMEBUFFER_COMPLETE) {
      throw new RendererError(
        `this driver would not give a floating-point render target (framebuffer status ${status})`,
      );
    }
  }

  resize(width: number, height: number): void {
    if (width === this.width && height === this.height) return;
    if (width === 0 || height === 0) return;
    this.width = width;
    this.height = height;
    this.allocateTarget();
  }

  /**
   * Make room for `count` particles. Grows and never shrinks: a trajectory has
   * the same count on every frame, so the only reallocation is the first.
   */
  private reserve(count: number): void {
    if (count <= this.capacity) return;
    const gl = this.gl;
    for (const buffer of this.positionBuffers) {
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.bufferData(gl.ARRAY_BUFFER, count * 4, gl.STREAM_DRAW);
    }
    this.capacity = count;
  }

  draw(
    positions: Positions,
    viewProjection: Float32Array,
    settings: RenderSettings,
  ): void {
    const gl = this.gl;
    const { count } = positions;
    this.reserve(count);

    gl.bindFramebuffer(gl.FRAMEBUFFER, this.framebuffer);
    gl.viewport(0, 0, this.width, this.height);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);

    // No depth test, because the picture is a sum along the line of sight
    // rather than a question of what is in front. One and one, because that sum
    // is what additive blending is.
    gl.disable(gl.DEPTH_TEST);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE);

    if (count > 0) {
      // The length form of bufferSubData, so a partial upload needs no
      // subarray and therefore allocates nothing on a path that runs sixty
      // times a second.
      gl.bindBuffer(gl.ARRAY_BUFFER, this.positionBuffers[0]);
      gl.bufferSubData(gl.ARRAY_BUFFER, 0, positions.x, 0, count);
      gl.bindBuffer(gl.ARRAY_BUFFER, this.positionBuffers[1]);
      gl.bufferSubData(gl.ARRAY_BUFFER, 0, positions.y, 0, count);
      gl.bindBuffer(gl.ARRAY_BUFFER, this.positionBuffers[2]);
      gl.bufferSubData(gl.ARRAY_BUFFER, 0, positions.z, 0, count);

      gl.useProgram(this.pointProgram);
      gl.uniformMatrix4fv(this.viewProjectionAt, false, viewProjection);
      gl.uniform2f(this.viewportAt, this.width, this.height);
      gl.uniform1f(this.pointSizeAt, settings.pointSize);
      gl.uniform1f(this.minimumPointSizeAt, settings.minimumPointSize);
      gl.uniform1f(this.brightnessAt, settings.brightness);
      gl.uniform3f(
        this.colourAt,
        settings.colour.red,
        settings.colour.green,
        settings.colour.blue,
      );

      gl.bindVertexArray(this.particleArray);
      gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, count);
      gl.bindVertexArray(null);
    }

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, this.width, this.height);
    gl.disable(gl.BLEND);

    gl.useProgram(this.postProgram);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.colourTexture);
    gl.uniform1i(this.sceneAt, 0);
    gl.uniform1f(this.exposureAt, settings.exposure);
    gl.uniform1f(this.whitePointAt, settings.whitePoint);

    gl.bindVertexArray(this.postArray);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bindVertexArray(null);
  }

  dispose(): void {
    const gl = this.gl;
    gl.deleteFramebuffer(this.framebuffer);
    gl.deleteTexture(this.colourTexture);
    gl.deleteProgram(this.pointProgram);
    gl.deleteProgram(this.postProgram);
    gl.deleteBuffer(this.cornerBuffer);
    for (const buffer of this.positionBuffers) gl.deleteBuffer(buffer);
    gl.deleteVertexArray(this.particleArray);
    gl.deleteVertexArray(this.postArray);
  }
}

/** Start the WebGL2 backend, or say why it will not start. */
export function createWebGl2Renderer(canvas: HTMLCanvasElement): Renderer {
  const gl = canvas.getContext('webgl2', {
    alpha: false,
    antialias: false,
    depth: false,
    // The picture is redrawn in full on every frame, so nothing is gained by
    // keeping the last one and the driver is free to hand back any buffer.
    preserveDrawingBuffer: false,
    powerPreference: 'high-performance',
  });
  if (gl === null) throw new RendererError('this browser has no WebGL2');
  return new WebGl2Renderer(gl);
}
