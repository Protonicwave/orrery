/**
 * The WebGPU backend.
 *
 * The same two passes and the same arithmetic as the WebGL2 one, written in
 * WGSL. The shaders are transliterations of each other and should be read that
 * way: if a line here has no counterpart in `webgl2.ts`, one of the two is
 * wrong and the screenshot comparison in `web/tests/render/` is what says so.
 *
 * ## The three places the two backends genuinely differ
 *
 * The clip volume runs from zero to one in z rather than from minus one, which
 * the camera is told and which changes only the third row of the projection.
 *
 * A texture's second coordinate runs down the image here and up it in OpenGL,
 * so the full-screen pass reads its rows the other way round. Get this wrong
 * and the picture is upside down, which is the one difference between the
 * backends that is obvious rather than subtle.
 *
 * The API allocates on every frame by construction: the canvas hands out a new
 * texture each frame, so a view has to be made of it, and the work has to be
 * recorded into a command encoder that is then consumed. Those two objects are
 * the only allocations in this file's draw path. Everything the renderer itself
 * owns, the descriptors included, is made once and mutated in place.
 */

import type { DepthRange } from './camera';
import {
  type Positions,
  type Renderer,
  RendererError,
  type RenderSettings,
} from './renderer';

const POINT_SHADER = /* wgsl */ `
struct Settings {
    view_projection : mat4x4<f32>,
    viewport : vec2<f32>,
    point_size : f32,
    minimum_point_size : f32,
    colour : vec3<f32>,
    brightness : f32,
};

@group(0) @binding(0) var<uniform> settings : Settings;

struct Sprite {
    @builtin(position) position : vec4<f32>,
    @location(0) corner : vec2<f32>,
};

@vertex
fn vertex_main(
    @location(0) corner : vec2<f32>,
    @location(1) position_x : f32,
    @location(2) position_y : f32,
    @location(3) position_z : f32,
) -> Sprite {
    let clip = settings.view_projection * vec4<f32>(position_x, position_y, position_z, 1.0);

    // The guard on w keeps a particle level with or behind the eye from
    // producing a negative or enormous sprite.
    let diameter = max(settings.point_size / max(clip.w, 0.001), settings.minimum_point_size);

    // A diameter of d pixels is d / viewport in normalised device coordinates,
    // multiplied by w so that it survives the perspective divide.
    let offset = corner * (diameter / settings.viewport) * clip.w;

    var sprite : Sprite;
    sprite.position = vec4<f32>(clip.xy + offset, clip.z, clip.w);
    sprite.corner = corner;
    return sprite;
}

@fragment
fn fragment_main(@location(0) corner : vec2<f32>) -> @location(0) vec4<f32> {
    let radius_squared = dot(corner, corner);
    if (radius_squared > 1.0) {
        discard;
    }

    // A Gaussian with its value at the rim subtracted, which brings the edge to
    // exactly zero and keeps a hundred thousand sprite edges from reading as
    // texture in the faint parts of the image.
    let sharpness = 4.0;
    let falloff = exp(-sharpness * radius_squared) - exp(-sharpness);

    return vec4<f32>(settings.colour * settings.brightness * falloff, 1.0);
}
`;

const POST_SHADER = /* wgsl */ `
struct Tone {
    exposure : f32,
    white_point : f32,
};

@group(0) @binding(0) var scene : texture_2d<f32>;
@group(0) @binding(1) var scene_sampler : sampler;
@group(0) @binding(2) var<uniform> tone : Tone;

struct Screen {
    @builtin(position) position : vec4<f32>,
    @location(0) texture_coordinate : vec2<f32>,
};

@vertex
fn vertex_main(@builtin(vertex_index) index : u32) -> Screen {
    // One oversized triangle rather than two making a quad: the diagonal seam
    // of a quad is a place the rasteriser evaluates some pixels twice.
    let corner = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));

    var screen : Screen;
    screen.position = vec4<f32>((corner * 2.0) - 1.0, 0.0, 1.0);
    // The second coordinate runs down a texture here and up one in OpenGL.
    screen.texture_coordinate = vec2<f32>(corner.x, 1.0 - corner.y);
    return screen;
}

// The curve of include/orrery/viz/tone_map.hpp, transliterated.
fn orrery_tone_curve(radiance : vec3<f32>, exposure : f32, white_point : f32) -> vec3<f32> {
    let exposed = max(radiance * exposure, vec3<f32>(0.0));
    let mapped = exposed * (1.0 + exposed / (white_point * white_point)) / (1.0 + exposed);
    return clamp(mapped, vec3<f32>(0.0), vec3<f32>(1.0));
}

// IEC 61966-2-1, the same constants encode_srgb uses in that file.
fn encode_srgb(linear : vec3<f32>) -> vec3<f32> {
    let low = linear * 12.92;
    let high = (1.055 * pow(linear, vec3<f32>(1.0 / 2.4))) - 0.055;
    return select(high, low, linear <= vec3<f32>(0.0031308));
}

@fragment
fn fragment_main(@location(0) texture_coordinate : vec2<f32>) -> @location(0) vec4<f32> {
    let radiance = textureSample(scene, scene_sampler, texture_coordinate).rgb;
    return vec4<f32>(encode_srgb(orrery_tone_curve(radiance, tone.exposure, tone.white_point)), 1.0);
}
`;

/** The unit quad every sprite is an instance of, as a triangle strip. */
const CORNERS = new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]);

/** The accumulation target's format: a sum needs somewhere unbounded to go. */
const ACCUMULATION_FORMAT: GPUTextureFormat = 'rgba16float';

class WebGpuRenderer implements Renderer {
  readonly backend = 'webgpu' as const;
  readonly depthRange: DepthRange = 'zero-to-one';
  readonly description: string;

  private readonly device: GPUDevice;
  private readonly context: GPUCanvasContext;
  private readonly pointPipeline: GPURenderPipeline;
  private readonly postPipeline: GPURenderPipeline;
  private readonly cornerBuffer: GPUBuffer;
  private readonly settingsBuffer: GPUBuffer;
  private readonly toneBuffer: GPUBuffer;
  private readonly sampler: GPUSampler;
  private readonly pointBindGroup: GPUBindGroup;

  /** Written on every frame, uploaded in one go. See the layout note below. */
  private readonly settings = new Float32Array(24);
  private readonly tone = new Float32Array(4);

  private positionBuffers: [GPUBuffer, GPUBuffer, GPUBuffer] | null = null;
  private capacity = 0;

  private accumulation: GPUTexture | null = null;
  private accumulationView: GPUTextureView | null = null;
  private postBindGroup: GPUBindGroup | null = null;

  private width = 1;
  private height = 1;

  // The descriptors are made once and mutated, rather than written as object
  // literals inside draw. Two render passes a frame at sixty frames a second is
  // seven thousand descriptors a minute otherwise, all of them identical apart
  // from one view.
  private readonly accumulationAttachment: GPURenderPassColorAttachment = {
    view: undefined as unknown as GPUTextureView,
    clearValue: { r: 0, g: 0, b: 0, a: 1 },
    loadOp: 'clear',
    storeOp: 'store',
  };
  private readonly postAttachment: GPURenderPassColorAttachment = {
    view: undefined as unknown as GPUTextureView,
    clearValue: { r: 0, g: 0, b: 0, a: 1 },
    loadOp: 'clear',
    storeOp: 'store',
  };
  private readonly accumulationPass: GPURenderPassDescriptor = {
    colorAttachments: [this.accumulationAttachment],
  };
  private readonly postPass: GPURenderPassDescriptor = {
    colorAttachments: [this.postAttachment],
  };
  private readonly submission: GPUCommandBuffer[] = [
    undefined as unknown as GPUCommandBuffer,
  ];

  constructor(
    device: GPUDevice,
    context: GPUCanvasContext,
    format: GPUTextureFormat,
    description: string,
  ) {
    this.device = device;
    this.context = context;
    this.description = description;

    const pointModule = device.createShaderModule({ code: POINT_SHADER });
    const postModule = device.createShaderModule({ code: POST_SHADER });

    this.pointPipeline = device.createRenderPipeline({
      layout: 'auto',
      vertex: {
        module: pointModule,
        entryPoint: 'vertex_main',
        buffers: [
          {
            arrayStride: 8,
            stepMode: 'vertex',
            attributes: [{ shaderLocation: 0, offset: 0, format: 'float32x2' }],
          },
          // One float per particle from a buffer of its own, three times over,
          // which is what a component array is. No interleaving pass and no
          // gather, as ADR-0004 arranged on the other side.
          {
            arrayStride: 4,
            stepMode: 'instance',
            attributes: [{ shaderLocation: 1, offset: 0, format: 'float32' }],
          },
          {
            arrayStride: 4,
            stepMode: 'instance',
            attributes: [{ shaderLocation: 2, offset: 0, format: 'float32' }],
          },
          {
            arrayStride: 4,
            stepMode: 'instance',
            attributes: [{ shaderLocation: 3, offset: 0, format: 'float32' }],
          },
        ],
      },
      fragment: {
        module: pointModule,
        entryPoint: 'fragment_main',
        targets: [
          {
            format: ACCUMULATION_FORMAT,
            // One and one: the sum along a line of sight is what additive
            // blending is, and there is no depth test for the same reason.
            blend: {
              color: { srcFactor: 'one', dstFactor: 'one', operation: 'add' },
              alpha: { srcFactor: 'one', dstFactor: 'one', operation: 'add' },
            },
          },
        ],
      },
      primitive: { topology: 'triangle-strip' },
    });

    this.postPipeline = device.createRenderPipeline({
      layout: 'auto',
      vertex: { module: postModule, entryPoint: 'vertex_main' },
      fragment: {
        module: postModule,
        entryPoint: 'fragment_main',
        targets: [{ format }],
      },
      primitive: { topology: 'triangle-list' },
    });

    this.cornerBuffer = device.createBuffer({
      size: CORNERS.byteLength,
      usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
    });
    device.queue.writeBuffer(this.cornerBuffer, 0, CORNERS);

    this.settingsBuffer = device.createBuffer({
      size: this.settings.byteLength,
      usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });
    this.toneBuffer = device.createBuffer({
      size: this.tone.byteLength,
      usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });

    // Nearest and clamped: the pass samples one texel per pixel, so there is
    // nothing for a linear filter to interpolate between.
    this.sampler = device.createSampler({
      magFilter: 'nearest',
      minFilter: 'nearest',
      addressModeU: 'clamp-to-edge',
      addressModeV: 'clamp-to-edge',
    });

    this.pointBindGroup = device.createBindGroup({
      layout: this.pointPipeline.getBindGroupLayout(0),
      entries: [{ binding: 0, resource: { buffer: this.settingsBuffer } }],
    });

    this.allocateTarget();
  }

  private allocateTarget(): void {
    this.accumulation?.destroy();
    this.accumulation = this.device.createTexture({
      size: { width: this.width, height: this.height },
      format: ACCUMULATION_FORMAT,
      usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING,
    });
    this.accumulationView = this.accumulation.createView();
    this.accumulationAttachment.view = this.accumulationView;

    this.postBindGroup = this.device.createBindGroup({
      layout: this.postPipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: this.accumulationView },
        { binding: 1, resource: this.sampler },
        { binding: 2, resource: { buffer: this.toneBuffer } },
      ],
    });
  }

  resize(width: number, height: number): void {
    if (width === this.width && height === this.height) return;
    if (width === 0 || height === 0) return;
    this.width = width;
    this.height = height;
    this.allocateTarget();
  }

  /** Grows and never shrinks; a trajectory has one particle count throughout. */
  private reserve(count: number): void {
    if (count <= this.capacity && this.positionBuffers !== null) return;
    for (const buffer of this.positionBuffers ?? []) buffer.destroy();

    const make = () =>
      this.device.createBuffer({
        size: Math.max(count, 1) * 4,
        usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
      });
    this.positionBuffers = [make(), make(), make()];
    this.capacity = count;
  }

  draw(
    positions: Positions,
    viewProjection: Float32Array,
    settings: RenderSettings,
  ): void {
    const { count } = positions;
    this.reserve(count);
    const buffers = this.positionBuffers as [GPUBuffer, GPUBuffer, GPUBuffer];
    const { queue } = this.device;

    // The uniform block, laid out as WGSL packs it: the matrix at 0, the
    // viewport at 64, the two sizes after it, and the colour at 80 because a
    // vec3 aligns to sixteen bytes.
    this.settings.set(viewProjection, 0);
    this.settings[16] = this.width;
    this.settings[17] = this.height;
    this.settings[18] = settings.pointSize;
    this.settings[19] = settings.minimumPointSize;
    this.settings[20] = settings.colour.red;
    this.settings[21] = settings.colour.green;
    this.settings[22] = settings.colour.blue;
    this.settings[23] = settings.brightness;
    queue.writeBuffer(this.settingsBuffer, 0, this.settings);

    this.tone[0] = settings.exposure;
    this.tone[1] = settings.whitePoint;
    queue.writeBuffer(this.toneBuffer, 0, this.tone);

    if (count > 0) {
      // The offset and length form, so a partial upload needs no subarray.
      queue.writeBuffer(
        buffers[0],
        0,
        positions.x.buffer,
        positions.x.byteOffset,
        count * 4,
      );
      queue.writeBuffer(
        buffers[1],
        0,
        positions.y.buffer,
        positions.y.byteOffset,
        count * 4,
      );
      queue.writeBuffer(
        buffers[2],
        0,
        positions.z.buffer,
        positions.z.byteOffset,
        count * 4,
      );
    }

    const encoder = this.device.createCommandEncoder();

    const accumulate = encoder.beginRenderPass(this.accumulationPass);
    if (count > 0) {
      accumulate.setPipeline(this.pointPipeline);
      accumulate.setBindGroup(0, this.pointBindGroup);
      accumulate.setVertexBuffer(0, this.cornerBuffer);
      accumulate.setVertexBuffer(1, buffers[0]);
      accumulate.setVertexBuffer(2, buffers[1]);
      accumulate.setVertexBuffer(3, buffers[2]);
      accumulate.draw(4, count);
    }
    accumulate.end();

    this.postAttachment.view = this.context.getCurrentTexture().createView();
    const present = encoder.beginRenderPass(this.postPass);
    present.setPipeline(this.postPipeline);
    present.setBindGroup(0, this.postBindGroup as GPUBindGroup);
    present.draw(3);
    present.end();

    this.submission[0] = encoder.finish();
    queue.submit(this.submission);
  }

  dispose(): void {
    this.accumulation?.destroy();
    for (const buffer of this.positionBuffers ?? []) buffer.destroy();
    this.cornerBuffer.destroy();
    this.settingsBuffer.destroy();
    this.toneBuffer.destroy();
    this.device.destroy();
  }
}

/** Start the WebGPU backend, or say why it will not start. */
export async function createWebGpuRenderer(
  canvas: HTMLCanvasElement,
): Promise<Renderer> {
  if (navigator.gpu === undefined) {
    throw new RendererError('this browser has no WebGPU');
  }

  const adapter = await navigator.gpu.requestAdapter({
    powerPreference: 'high-performance',
  });
  if (adapter === null) {
    throw new RendererError('WebGPU is present but no adapter would be given');
  }

  const device = await adapter.requestDevice();
  const context = canvas.getContext('webgpu');
  if (context === null) {
    throw new RendererError('this canvas would not give a WebGPU context');
  }

  const format = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: 'opaque' });

  // The sRGB encoding is applied in the shader rather than by asking for an
  // sRGB canvas format, so that this backend and the WebGL2 one, which has no
  // such format to ask for, write the same bytes.
  const info = adapter.info as GPUAdapterInfo | undefined;
  const named = [info?.vendor, info?.architecture, info?.description]
    .filter((part) => part !== undefined && part !== '')
    .join(' ');

  return new WebGpuRenderer(
    device,
    context,
    format,
    `WebGPU${named === '' ? '' : `, ${named}`}`,
  );
}
