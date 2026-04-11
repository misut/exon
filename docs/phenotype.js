// phenotype.js — WASI polyfill + WebGPU 2D renderer
//
// Usage:
//   import { mount } from './phenotype.js';
//   mount('hello.wasm');

// --- Draw command opcodes (must match phenotype.cppm Cmd enum) ---

const CMD_CLEAR      = 1;
const CMD_FILL_RECT  = 2;
const CMD_STROKE_RECT = 3;
const CMD_ROUND_RECT = 4;
const CMD_DRAW_TEXT  = 5;
const CMD_DRAW_LINE  = 6;
const CMD_HIT_REGION = 7;

// --- Color helpers ---

function unpackColor(rgba) {
  return {
    r: ((rgba >>> 24) & 0xFF) / 255,
    g: ((rgba >>> 16) & 0xFF) / 255,
    b: ((rgba >>>  8) & 0xFF) / 255,
    a: ((rgba >>>  0) & 0xFF) / 255,
  };
}

// --- Text measurement (offscreen canvas) ---

const measureCanvas = new OffscreenCanvas(1, 1);
const measureCtx = measureCanvas.getContext('2d');

const FONT_SANS = 'system-ui, -apple-system, "Segoe UI", Roboto, sans-serif';
const FONT_MONO = '"SF Mono", "Fira Code", "Cascadia Code", monospace';

function fontString(size, mono) {
  return `${size}px ${mono ? FONT_MONO : FONT_SANS}`;
}

// --- WGSL Shaders ---

const SHADER_CODE = /* wgsl */`
struct Uniforms {
  viewport: vec2f,
  _pad: vec2f,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;

// --- Color pipeline ---

struct ColorInstance {
  @location(0) rect: vec4f,    // x, y, w, h
  @location(1) color: vec4f,   // r, g, b, a
  @location(2) params: vec4f,  // corner_radius, border_width, type(0=fill,1=stroke,2=round,3=line), 0
};

struct ColorVsOut {
  @builtin(position) pos: vec4f,
  @location(0) color: vec4f,
  @location(1) local_pos: vec2f,
  @location(2) rect_size: vec2f,
  @location(3) params: vec4f,
};

@vertex fn vs_color(
  @builtin(vertex_index) vi: u32,
  @builtin(instance_index) ii: u32,
  inst: ColorInstance,
) -> ColorVsOut {
  // 6 vertices for a quad (two triangles)
  var corners = array<vec2f, 6>(
    vec2f(0, 0), vec2f(1, 0), vec2f(0, 1),
    vec2f(1, 0), vec2f(1, 1), vec2f(0, 1),
  );
  let c = corners[vi];
  let px = inst.rect.x + c.x * inst.rect.z;
  let py = inst.rect.y + c.y * inst.rect.w;
  // Convert pixel coords to clip space: [0, width] -> [-1, 1]
  let cx = (px / uniforms.viewport.x) * 2.0 - 1.0;
  let cy = 1.0 - (py / uniforms.viewport.y) * 2.0;

  var out: ColorVsOut;
  out.pos = vec4f(cx, cy, 0, 1);
  out.color = inst.color;
  out.local_pos = c * inst.rect.zw; // local pixel coords within rect
  out.rect_size = inst.rect.zw;
  out.params = inst.params;
  return out;
}

@fragment fn fs_color(in: ColorVsOut) -> @location(0) vec4f {
  let draw_type = u32(in.params.z);
  let radius = in.params.x;
  let border_w = in.params.y;

  // Rounded rect SDF
  if (draw_type == 2u && radius > 0.0) {
    let half_size = in.rect_size * 0.5;
    let p = abs(in.local_pos - half_size) - half_size + vec2f(radius);
    let d = length(max(p, vec2f(0.0))) - radius;
    if (d > 0.5) { discard; }
    let alpha = in.color.a * clamp(0.5 - d, 0.0, 1.0);
    return vec4f(in.color.rgb * alpha, alpha);
  }

  // Stroke rect
  if (draw_type == 1u) {
    let lp = in.local_pos;
    let sz = in.rect_size;
    let bw = border_w;
    if (lp.x > bw && lp.x < sz.x - bw && lp.y > bw && lp.y < sz.y - bw) {
      discard;
    }
    return in.color;
  }

  // Line (rendered as thin rect, just fill)
  if (draw_type == 3u) {
    return in.color;
  }

  // Default fill
  return in.color;
}

// --- Texture pipeline (text) ---

struct TextInstance {
  @location(0) rect: vec4f,     // x, y, w, h in screen pixels
  @location(1) uv_rect: vec4f,  // u, v, uw, vh in [0,1]
};

struct TextVsOut {
  @builtin(position) pos: vec4f,
  @location(0) uv: vec2f,
};

@vertex fn vs_text(
  @builtin(vertex_index) vi: u32,
  inst: TextInstance,
) -> TextVsOut {
  var corners = array<vec2f, 6>(
    vec2f(0, 0), vec2f(1, 0), vec2f(0, 1),
    vec2f(1, 0), vec2f(1, 1), vec2f(0, 1),
  );
  let c = corners[vi];
  let px = inst.rect.x + c.x * inst.rect.z;
  let py = inst.rect.y + c.y * inst.rect.w;
  let cx = (px / uniforms.viewport.x) * 2.0 - 1.0;
  let cy = 1.0 - (py / uniforms.viewport.y) * 2.0;

  var out: TextVsOut;
  out.pos = vec4f(cx, cy, 0, 1);
  out.uv = inst.uv_rect.xy + c * inst.uv_rect.zw;
  return out;
}

@group(0) @binding(1) var textAtlas: texture_2d<f32>;
@group(0) @binding(2) var textSampler: sampler;

@fragment fn fs_text(in: TextVsOut) -> @location(0) vec4f {
  let sample = textureSample(textAtlas, textSampler, in.uv);
  if (sample.a < 0.01) { discard; }
  return sample;
}
`;

// --- WebGPU Renderer ---

function createRenderer(device, context, canvas, format) {
  const shaderModule = device.createShaderModule({ code: SHADER_CODE });

  // Uniform buffer
  const uniformBuffer = device.createBuffer({
    size: 16, // vec2f viewport + vec2f pad
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
  });

  // Bind group layout
  const bindGroupLayout = device.createBindGroupLayout({
    entries: [
      { binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
        buffer: { type: 'uniform' } },
      { binding: 1, visibility: GPUShaderStage.FRAGMENT,
        texture: { sampleType: 'float' } },
      { binding: 2, visibility: GPUShaderStage.FRAGMENT,
        sampler: { type: 'filtering' } },
    ],
  });

  const pipelineLayout = device.createPipelineLayout({
    bindGroupLayouts: [bindGroupLayout],
  });

  // Color pipeline
  const colorPipeline = device.createRenderPipeline({
    layout: pipelineLayout,
    vertex: {
      module: shaderModule,
      entryPoint: 'vs_color',
      buffers: [{
        arrayStride: 48, // 4+4+4 vec4f = 12 floats = 48 bytes
        stepMode: 'instance',
        attributes: [
          { shaderLocation: 0, offset: 0, format: 'float32x4' },  // rect
          { shaderLocation: 1, offset: 16, format: 'float32x4' }, // color
          { shaderLocation: 2, offset: 32, format: 'float32x4' }, // params
        ],
      }],
    },
    fragment: {
      module: shaderModule,
      entryPoint: 'fs_color',
      targets: [{ format, blend: {
        color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha' },
        alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' },
      }}],
    },
    primitive: { topology: 'triangle-list' },
  });

  // Text pipeline
  const textPipeline = device.createRenderPipeline({
    layout: pipelineLayout,
    vertex: {
      module: shaderModule,
      entryPoint: 'vs_text',
      buffers: [{
        arrayStride: 32, // 2 vec4f = 8 floats = 32 bytes
        stepMode: 'instance',
        attributes: [
          { shaderLocation: 0, offset: 0, format: 'float32x4' },  // rect
          { shaderLocation: 1, offset: 16, format: 'float32x4' }, // uv_rect
        ],
      }],
    },
    fragment: {
      module: shaderModule,
      entryPoint: 'fs_text',
      targets: [{ format, blend: {
        color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha' },
        alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' },
      }}],
    },
    primitive: { topology: 'triangle-list' },
  });

  // Text sampler
  const textSampler = device.createSampler({
    magFilter: 'linear',
    minFilter: 'linear',
  });

  // Placeholder 1x1 white texture (used when no text)
  let textAtlasTexture = device.createTexture({
    size: [1, 1],
    format: 'rgba8unorm',
    usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST |
           GPUTextureUsage.RENDER_ATTACHMENT,
  });
  device.queue.writeTexture(
    { texture: textAtlasTexture },
    new Uint8Array([255, 255, 255, 255]),
    { bytesPerRow: 4 },
    [1, 1],
  );

  let bindGroup = createBindGroup(uniformBuffer, textAtlasTexture, textSampler);

  function createBindGroup(ub, tex, samp) {
    return device.createBindGroup({
      layout: bindGroupLayout,
      entries: [
        { binding: 0, resource: { buffer: ub } },
        { binding: 1, resource: tex.createView() },
        { binding: 2, resource: samp },
      ],
    });
  }

  // Text atlas offscreen canvas
  const ATLAS_SIZE = 4096;
  const atlasCanvas = new OffscreenCanvas(ATLAS_SIZE, ATLAS_SIZE);
  const atlasCtx = atlasCanvas.getContext('2d');

  return {
    render(colorQuads, textEntries, clearColor) {
      const dpr = window.devicePixelRatio || 1;
      const w = canvas.clientWidth;
      const h = canvas.clientHeight;
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);

      // Update uniforms (logical pixels)
      device.queue.writeBuffer(uniformBuffer, 0,
        new Float32Array([w, h, 0, 0]));

      // --- Build text atlas ---
      let textInstanceData = null;
      let textInstanceCount = 0;

      if (textEntries.length > 0) {
        atlasCtx.clearRect(0, 0, ATLAS_SIZE, ATLAS_SIZE);

        let ax = 0, ay = 0;
        let rowHeight = 0;
        const textQuads = [];
        const padding = 2;

        for (const t of textEntries) {
          const font = fontString(t.fontSize * dpr, t.mono);
          atlasCtx.font = font;
          const metrics = atlasCtx.measureText(t.text);
          const tw = Math.ceil(metrics.width) + padding * 2;
          const th = Math.ceil(t.fontSize * dpr * 1.4) + padding * 2;

          // Wrap to next row
          if (ax + tw > ATLAS_SIZE) {
            ax = 0;
            ay += rowHeight;
            rowHeight = 0;
          }

          if (ay + th > ATLAS_SIZE) break; // atlas full

          // Draw text
          atlasCtx.font = font; // re-set after potential clear
          const { r, g, b, a } = t.color;
          atlasCtx.fillStyle = `rgba(${Math.round(r*255)},${Math.round(g*255)},${Math.round(b*255)},${a})`;
          atlasCtx.textBaseline = 'top';
          atlasCtx.fillText(t.text, ax + padding, ay + padding);

          textQuads.push({
            // Screen rect (logical pixels)
            x: t.x, y: t.y,
            w: (tw - padding * 2) / dpr,
            h: (th - padding * 2) / dpr,
            // UV rect
            u: ax / ATLAS_SIZE,
            v: ay / ATLAS_SIZE,
            uw: tw / ATLAS_SIZE,
            vh: th / ATLAS_SIZE,
          });

          ax += tw;
          if (th > rowHeight) rowHeight = th;
        }

        // Upload atlas to GPU
        if (textAtlasTexture.width !== ATLAS_SIZE || textAtlasTexture.height !== ATLAS_SIZE) {
          textAtlasTexture.destroy();
          textAtlasTexture = device.createTexture({
            size: [ATLAS_SIZE, ATLAS_SIZE],
            format: 'rgba8unorm',
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST |
                   GPUTextureUsage.RENDER_ATTACHMENT,
          });
        }

        device.queue.copyExternalImageToTexture(
          { source: atlasCanvas },
          { texture: textAtlasTexture },
          [ATLAS_SIZE, ATLAS_SIZE],
        );

        bindGroup = createBindGroup(uniformBuffer, textAtlasTexture, textSampler);

        // Build text instance buffer
        textInstanceCount = textQuads.length;
        const textData = new Float32Array(textInstanceCount * 8);
        for (let i = 0; i < textInstanceCount; i++) {
          const q = textQuads[i];
          textData[i * 8 + 0] = q.x;
          textData[i * 8 + 1] = q.y;
          textData[i * 8 + 2] = q.w;
          textData[i * 8 + 3] = q.h;
          textData[i * 8 + 4] = q.u;
          textData[i * 8 + 5] = q.v;
          textData[i * 8 + 6] = q.uw;
          textData[i * 8 + 7] = q.vh;
        }
        textInstanceData = textData;
      }

      // --- Build color instance buffer ---
      const colorCount = colorQuads.length;
      const colorData = new Float32Array(colorCount * 12);
      for (let i = 0; i < colorCount; i++) {
        const q = colorQuads[i];
        // rect
        colorData[i * 12 + 0] = q.x;
        colorData[i * 12 + 1] = q.y;
        colorData[i * 12 + 2] = q.w;
        colorData[i * 12 + 3] = q.h;
        // color
        colorData[i * 12 + 4] = q.r;
        colorData[i * 12 + 5] = q.g;
        colorData[i * 12 + 6] = q.b;
        colorData[i * 12 + 7] = q.a;
        // params: radius, border_width, type, 0
        colorData[i * 12 + 8] = q.radius || 0;
        colorData[i * 12 + 9] = q.borderWidth || 0;
        colorData[i * 12 + 10] = q.type || 0;
        colorData[i * 12 + 11] = 0;
      }

      // --- GPU render ---
      const encoder = device.createCommandEncoder();
      const pass = encoder.beginRenderPass({
        colorAttachments: [{
          view: context.getCurrentTexture().createView(),
          clearValue: clearColor,
          loadOp: 'clear',
          storeOp: 'store',
        }],
      });

      pass.setBindGroup(0, bindGroup);

      // Color quads
      if (colorCount > 0) {
        const colorBuf = device.createBuffer({
          size: colorData.byteLength,
          usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
          mappedAtCreation: true,
        });
        new Float32Array(colorBuf.getMappedRange()).set(colorData);
        colorBuf.unmap();
        pass.setPipeline(colorPipeline);
        pass.setVertexBuffer(0, colorBuf);
        pass.draw(6, colorCount);
      }

      // Text quads
      if (textInstanceCount > 0 && textInstanceData) {
        const textBuf = device.createBuffer({
          size: textInstanceData.byteLength,
          usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
          mappedAtCreation: true,
        });
        new Float32Array(textBuf.getMappedRange()).set(textInstanceData);
        textBuf.unmap();
        pass.setPipeline(textPipeline);
        pass.setVertexBuffer(0, textBuf);
        pass.draw(6, textInstanceCount);
      }

      pass.end();
      device.queue.submit([encoder.finish()]);
    },
  };
}

// --- Command buffer parser ---

function parseCommands(instance) {
  const mem = instance.exports.memory.buffer;
  const bufOffset = instance.exports.phenotype_get_cmd_buf();
  const bufLen = instance.exports.phenotype_get_cmd_len();
  if (bufLen === 0) return { quads: [], texts: [], hitRegions: [], clearColor: { r: 0.98, g: 0.98, b: 0.98, a: 1 } };

  const bytes = new Uint8Array(mem);
  const view = new DataView(mem);
  let pos = bufOffset;
  const end = bufOffset + bufLen;
  const decoder = new TextDecoder();

  function readStr(offset, len) {
    return decoder.decode(bytes.slice(offset, offset + len));
  }
  function align4(p) {
    return (p + 3) & ~3;
  }

  const quads = [];
  const texts = [];
  const hitRegions = [];
  let clearColor = { r: 0.98, g: 0.98, b: 0.98, a: 1 };

  while (pos < end) {
    const cmd = view.getUint32(pos, true); pos += 4;

    switch (cmd) {
      case CMD_CLEAR: {
        const rgba = view.getUint32(pos, true); pos += 4;
        clearColor = unpackColor(rgba);
        break;
      }
      case CMD_FILL_RECT: {
        const x = view.getFloat32(pos, true); pos += 4;
        const y = view.getFloat32(pos, true); pos += 4;
        const w = view.getFloat32(pos, true); pos += 4;
        const h = view.getFloat32(pos, true); pos += 4;
        const rgba = view.getUint32(pos, true); pos += 4;
        const c = unpackColor(rgba);
        quads.push({ x, y, w, h, r: c.r, g: c.g, b: c.b, a: c.a, type: 0 });
        break;
      }
      case CMD_STROKE_RECT: {
        const x = view.getFloat32(pos, true); pos += 4;
        const y = view.getFloat32(pos, true); pos += 4;
        const w = view.getFloat32(pos, true); pos += 4;
        const h = view.getFloat32(pos, true); pos += 4;
        const lw = view.getFloat32(pos, true); pos += 4;
        const rgba = view.getUint32(pos, true); pos += 4;
        const c = unpackColor(rgba);
        quads.push({ x, y, w, h, r: c.r, g: c.g, b: c.b, a: c.a, type: 1, borderWidth: lw });
        break;
      }
      case CMD_ROUND_RECT: {
        const x = view.getFloat32(pos, true); pos += 4;
        const y = view.getFloat32(pos, true); pos += 4;
        const w = view.getFloat32(pos, true); pos += 4;
        const h = view.getFloat32(pos, true); pos += 4;
        const radius = view.getFloat32(pos, true); pos += 4;
        const rgba = view.getUint32(pos, true); pos += 4;
        const c = unpackColor(rgba);
        quads.push({ x, y, w, h, r: c.r, g: c.g, b: c.b, a: c.a, type: 2, radius });
        break;
      }
      case CMD_DRAW_TEXT: {
        const x = view.getFloat32(pos, true); pos += 4;
        const y = view.getFloat32(pos, true); pos += 4;
        const fontSize = view.getFloat32(pos, true); pos += 4;
        const mono = view.getUint32(pos, true); pos += 4;
        const rgba = view.getUint32(pos, true); pos += 4;
        const len = view.getUint32(pos, true); pos += 4;
        const text = readStr(pos, len);
        pos = align4(pos + len);
        const c = unpackColor(rgba);
        texts.push({ x, y, fontSize, mono: !!mono, text, color: c });
        break;
      }
      case CMD_DRAW_LINE: {
        const x1 = view.getFloat32(pos, true); pos += 4;
        const y1 = view.getFloat32(pos, true); pos += 4;
        const x2 = view.getFloat32(pos, true); pos += 4;
        const y2 = view.getFloat32(pos, true); pos += 4;
        const thickness = view.getFloat32(pos, true); pos += 4;
        const rgba = view.getUint32(pos, true); pos += 4;
        const c = unpackColor(rgba);
        // Represent line as a thin rect
        const dx = x2 - x1, dy = y2 - y1;
        const len = Math.sqrt(dx * dx + dy * dy);
        if (Math.abs(dy) < 0.01) {
          // Horizontal line
          quads.push({ x: Math.min(x1, x2), y: y1 - thickness / 2, w: Math.abs(dx), h: thickness,
            r: c.r, g: c.g, b: c.b, a: c.a, type: 3 });
        } else if (Math.abs(dx) < 0.01) {
          // Vertical line
          quads.push({ x: x1 - thickness / 2, y: Math.min(y1, y2), w: thickness, h: Math.abs(dy),
            r: c.r, g: c.g, b: c.b, a: c.a, type: 3 });
        }
        // Diagonal lines not needed for UI
        break;
      }
      case CMD_HIT_REGION: {
        const x = view.getFloat32(pos, true); pos += 4;
        const y = view.getFloat32(pos, true); pos += 4;
        const w = view.getFloat32(pos, true); pos += 4;
        const h = view.getFloat32(pos, true); pos += 4;
        const callbackId = view.getUint32(pos, true); pos += 4;
        const cursorType = view.getUint32(pos, true); pos += 4;
        hitRegions.push({ x, y, w, h, callbackId, cursorType });
        break;
      }
      default:
        console.error(`phenotype: unknown command ${cmd} at offset ${pos - 4}`);
        return { quads, texts, hitRegions, clearColor };
    }
  }

  return { quads, texts, hitRegions, clearColor };
}

// --- Loader ---

export async function mount(wasmUrl, rootElement = document.body) {
  try {
  // --- WebGPU init ---
  if (!navigator.gpu) {
    rootElement.textContent = 'WebGPU is not supported in this browser.';
    return;
  }

  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) {
    rootElement.textContent = 'Failed to get WebGPU adapter.';
    return;
  }

  const device = await adapter.requestDevice();
  const canvas = document.createElement('canvas');
  canvas.style.width = '100%';
  canvas.style.height = '100vh';
  canvas.style.display = 'block';
  rootElement.appendChild(canvas);

  // Hidden input proxy — the real IME composition target and the
  // visible editor while a phenotype text field has focus. The
  // canvas itself is not an editable element, so the browser refuses
  // to start an IME composition session against it; routing keys
  // through a real <input> lets the browser engage the OS IME
  // normally and deliver composed text via input / compositionend
  // events.
  const hiddenInput = document.createElement('input');
  hiddenInput.type = 'text';
  hiddenInput.setAttribute('autocomplete', 'off');
  hiddenInput.setAttribute('autocorrect', 'off');
  hiddenInput.setAttribute('autocapitalize', 'off');
  hiddenInput.setAttribute('spellcheck', 'false');
  hiddenInput.style.position = 'absolute';
  hiddenInput.style.top = '-9999px';
  hiddenInput.style.left = '-9999px';
  hiddenInput.style.width = '1px';
  hiddenInput.style.height = '1px';
  hiddenInput.style.opacity = '0';
  hiddenInput.style.border = '0';
  hiddenInput.style.padding = '0';
  hiddenInput.style.outline = '0';
  hiddenInput.style.zIndex = '-1';
  rootElement.appendChild(hiddenInput);

  const gpuContext = canvas.getContext('webgpu');
  const format = navigator.gpu.getPreferredCanvasFormat();
  gpuContext.configure({ device, format, alphaMode: 'premultiplied' });

  const renderer = createRenderer(device, gpuContext, canvas, format);

  // --- WASM state ---
  let inst = null;
  let hitRegions = [];
  let scrollY = 0;
  let totalHeight = 0;

  function getMemory() { return inst.exports.memory; }

  function doFlush() {
    const parsed = parseCommands(inst);
    hitRegions = parsed.hitRegions;
    renderer.render(parsed.quads, parsed.texts, parsed.clearColor);
  }

  // --- WASI polyfill ---
  const wasiImports = {
    fd_write(fd, iovs_ptr, iovs_len, nwritten_ptr) {
      const mem = new DataView(getMemory().buffer);
      const bytes = new Uint8Array(getMemory().buffer);
      let total = 0;
      for (let i = 0; i < iovs_len; i++) {
        const ptr = mem.getUint32(iovs_ptr + i * 8, true);
        const len = mem.getUint32(iovs_ptr + i * 8 + 4, true);
        const chunk = bytes.slice(ptr, ptr + len);
        const text = new TextDecoder().decode(chunk);
        if (fd === 1) console.log(text);
        else if (fd === 2) console.error(text);
        total += len;
      }
      mem.setUint32(nwritten_ptr, total, true);
      return 0;
    },
    fd_close() { return 0; },
    fd_seek() { return 0; },
    fd_fdstat_get() { return 0; },
    fd_prestat_get() { return 8; },
    fd_prestat_dir_name() { return 8; },
    environ_get() { return 0; },
    environ_sizes_get(count_ptr, size_ptr) {
      const mem = new DataView(getMemory().buffer);
      mem.setUint32(count_ptr, 0, true);
      mem.setUint32(size_ptr, 0, true);
      return 0;
    },
    clock_time_get(id, precision, time_ptr) {
      const mem = new DataView(getMemory().buffer);
      const now = BigInt(Math.floor(performance.now() * 1e6));
      mem.setBigUint64(time_ptr, now, true);
      return 0;
    },
    proc_exit(code) {
      if (code !== 0) console.error(`proc_exit(${code})`);
    },
    sched_yield() { return 0; },
    random_get(buf_ptr, buf_len) {
      const bytes = new Uint8Array(getMemory().buffer, buf_ptr, buf_len);
      crypto.getRandomValues(bytes);
      return 0;
    },
  };

  // --- Phenotype WASM imports ---
  const phenotypeImports = {
    flush() { doFlush(); },

    measure_text(fontSize, mono, textPtr, textLen) {
      const bytes = new Uint8Array(getMemory().buffer);
      const text = new TextDecoder().decode(bytes.slice(textPtr, textPtr + textLen));
      measureCtx.font = fontString(fontSize, mono);
      return measureCtx.measureText(text).width;
    },

    get_canvas_width() {
      return canvas.clientWidth;
    },

    get_canvas_height() {
      return canvas.clientHeight;
    },

    open_url(urlPtr, urlLen) {
      const bytes = new Uint8Array(getMemory().buffer);
      const url = new TextDecoder().decode(bytes.slice(urlPtr, urlPtr + urlLen));
      window.open(url, '_blank');
    },
  };

  // --- Instantiate WASM ---
  const result = await WebAssembly.instantiateStreaming(
    fetch(wasmUrl),
    {
      wasi_snapshot_preview1: wasiImports,
      phenotype: phenotypeImports,
    }
  );
  inst = result.instance;

  if (inst.exports._start) {
    inst.exports._start();
  }

  if (inst.exports.phenotype_get_total_height) {
    totalHeight = inst.exports.phenotype_get_total_height();
  }

  // --- Events ---

  function hitTest(clientX, clientY) {
    const rect = canvas.getBoundingClientRect();
    const x = clientX - rect.left;
    const y = clientY - rect.top + scrollY;
    for (let i = hitRegions.length - 1; i >= 0; i--) {
      const hr = hitRegions[i];
      if (x >= hr.x && x <= hr.x + hr.w && y >= hr.y && y <= hr.y + hr.h) {
        return hr;
      }
    }
    return null;
  }

  // Focus & click
  let focusedId = 0xFFFFFFFF;
  canvas.setAttribute('tabindex', '0');

  // Sync the hidden input element to the currently focused phenotype
  // text field. While a text field is focused, hiddenInput becomes
  // the *visible* editor: it sits exactly over the canvas-painted
  // field with a transparent background and matching font / padding,
  // and shows the OS IME's inline composition natively (Korean /
  // Japanese / Chinese all work as in any normal text input). The
  // canvas still paints the background, border, and focus ring; the
  // HTML input handles the text glyphs, caret, and selection.
  //
  // When no text field is focused, hiddenInput is parked off-screen
  // with opacity 0 so the canvas's own painting takes over.
  function syncHiddenInputForFocus() {
    // Only show the HTML overlay when the focused element is a real
    // text input. Buttons and links also receive focus (Tab cycle,
    // click) but they're not in app.input_handlers, so phenotype_-
    // focused_is_input() returns 0 for them. Without this guard the
    // hidden input would still be positioned over the button with
    // opacity 1, and the browser would draw its native caret '|' on
    // top of the +/- glyph.
    const isInput = inst.exports.phenotype_focused_is_input
      ? inst.exports.phenotype_focused_is_input() === 1
      : false;
    if (focusedId === 0xFFFFFFFF || !isInput
        || !inst.exports.phenotype_input_load_focused) {
      hiddenInput.style.top = '-9999px';
      hiddenInput.style.left = '-9999px';
      hiddenInput.style.opacity = '0';
      hiddenInput.value = '';
      return;
    }
    const hr = hitRegions.find((h) => h.callbackId === focusedId);
    if (!hr) {
      hiddenInput.style.top = '-9999px';
      hiddenInput.style.left = '-9999px';
      hiddenInput.style.opacity = '0';
      return;
    }

    const rect = canvas.getBoundingClientRect();
    // hitRegions are world-space (pre-scroll). Subtract scrollY to get
    // canvas-space, then add the canvas's screen offset.
    hiddenInput.style.left = `${rect.left + window.scrollX + hr.x}px`;
    hiddenInput.style.top = `${rect.top + window.scrollY + hr.y - scrollY}px`;
    hiddenInput.style.width = `${hr.w}px`;
    hiddenInput.style.height = `${hr.h}px`;

    // Visible overlay. Transparent background and zero border so the
    // canvas-painted background, border, and focus ring still show
    // through. Padding / font / color match the widget::text_field
    // defaults from src/phenotype.cppm so the text doesn't visually
    // jump when the field gains or loses focus.
    hiddenInput.style.opacity = '1';
    hiddenInput.style.background = 'transparent';
    hiddenInput.style.border = '0';
    hiddenInput.style.outline = 'none';
    hiddenInput.style.padding = '8px 12px';
    hiddenInput.style.margin = '0';
    hiddenInput.style.boxSizing = 'border-box';
    hiddenInput.style.font = `16px ${FONT_SANS}`;
    hiddenInput.style.color = 'rgb(26, 26, 26)';
    hiddenInput.style.caretColor = 'rgb(37, 99, 235)';
    hiddenInput.style.zIndex = '1';
    // pointer-events: none keeps mouse clicks routing to the canvas
    // (so canvas-side click → focus-change logic stays the source of
    // truth). Click-to-position-caret inside an already-focused field
    // is a small UX limitation; arrow / Home / End / Backspace still
    // work via the HTML input's native key handling.
    hiddenInput.style.pointerEvents = 'none';

    // Load the C++ side's current value so the user starts editing
    // from the existing text rather than an empty buffer.
    const len = inst.exports.phenotype_input_load_focused();
    if (len > 0) {
      const ptr = inst.exports.phenotype_input_buf();
      const bytes = new Uint8Array(inst.exports.memory.buffer, ptr, len);
      hiddenInput.value = new TextDecoder().decode(bytes);
    } else {
      hiddenInput.value = '';
    }
    // Caret to the end of the loaded content — natural place for the
    // user to keep typing.
    hiddenInput.setSelectionRange(hiddenInput.value.length, hiddenInput.value.length);
  }

  canvas.addEventListener('click', (e) => {
    hiddenInput.focus();
    const hr = hitTest(e.clientX, e.clientY);
    // Update focus
    const newFocus = hr ? hr.callbackId : 0xFFFFFFFF;
    if (newFocus !== focusedId) {
      focusedId = newFocus;
      if (inst.exports.phenotype_set_focus)
        inst.exports.phenotype_set_focus(focusedId);
      // Re-sync the HTML overlay to the new focus target. The C++
      // side's value is loaded into hiddenInput.value here so the
      // user starts editing from the existing text.
      syncHiddenInputForFocus();
    }
    if (hr && inst.exports.phenotype_handle_event) {
      inst.exports.phenotype_handle_event(hr.callbackId);
      if (inst.exports.phenotype_get_total_height)
        totalHeight = inst.exports.phenotype_get_total_height();
    }
  });

  // Hover. The cursor and hovered_id are derived from the pointer's
  // current canvas position, but the browser only fires pointermove when
  // the mouse actually moves. Scroll and resize repaint the layout
  // underneath a stationary mouse, so without re-evaluating after each
  // repaint the cursor stays stuck on whatever was last under the
  // pointer (e.g. a `pointer` cursor lingers after the button scrolls
  // out from under the mouse). applyHoverAt() centralizes the lookup
  // so pointermove / pointerleave / wheel / resize all share it.
  let currentHoverId = 0xFFFFFFFF;
  let lastClientX = -1;
  let lastClientY = -1;

  function applyHoverAt(clientX, clientY) {
    if (clientX < 0 || clientY < 0) return; // pointer not yet over canvas
    const hr = hitTest(clientX, clientY);
    canvas.style.cursor = (hr && hr.cursorType === 1) ? 'pointer' : 'default';
    const newHoverId = hr ? hr.callbackId : 0xFFFFFFFF;
    if (newHoverId !== currentHoverId) {
      currentHoverId = newHoverId;
      if (inst.exports.phenotype_set_hover)
        inst.exports.phenotype_set_hover(newHoverId);
    }
  }

  canvas.addEventListener('pointermove', (e) => {
    lastClientX = e.clientX;
    lastClientY = e.clientY;
    applyHoverAt(lastClientX, lastClientY);
  });

  canvas.addEventListener('pointerleave', () => {
    lastClientX = -1;
    lastClientY = -1;
    canvas.style.cursor = 'default';
    if (currentHoverId !== 0xFFFFFFFF) {
      currentHoverId = 0xFFFFFFFF;
      if (inst.exports.phenotype_set_hover)
        inst.exports.phenotype_set_hover(0xFFFFFFFF);
    }
  });

  // Text input — push hiddenInput.value to C++ on every change.
  //
  // hiddenInput is a real <input> element, so the browser's native
  // editing handles every keystroke (English / numbers / symbols),
  // every IME composition tick (Korean / Japanese / Chinese inline
  // preview), Backspace, arrow / Home / End cursor moves, paste, and
  // selection. We don't filter on e.isComposing because the input's
  // .value IS the source of truth: whatever it currently shows is
  // what should be in C++. Pushing the same string twice is
  // idempotent — phenotype_input_commit just runs the user's mapper
  // and triggers a rebuild, which the runner is already designed for.
  hiddenInput.addEventListener('input', () => {
    if (focusedId === 0xFFFFFFFF || !inst.exports.phenotype_input_commit) return;
    const utf8 = new TextEncoder().encode(hiddenInput.value);
    if (utf8.length > 4096) return; // matches PHENOTYPE_INPUT_BUF_SIZE
    const ptr = inst.exports.phenotype_input_buf();
    const buf = new Uint8Array(inst.exports.memory.buffer, ptr, utf8.length);
    buf.set(utf8);
    inst.exports.phenotype_input_commit(utf8.length);
  });

  // Keyboard — only Tab and Enter need explicit handling now. Every
  // character key (including IME jamo / kana) is delivered through
  // hiddenInput's native input event above, so the keydown handler
  // no longer routes character codepoints into phenotype_handle_key.
  // Backspace / arrows / Home / End are handled natively by the HTML
  // input while a text field is focused; outside text fields they're
  // no-ops anyway, so we drop the per-key forwarding entirely.
  hiddenInput.addEventListener('keydown', (e) => {
    if (e.key === 'Tab') {
      e.preventDefault();
      if (inst.exports.phenotype_handle_tab)
        inst.exports.phenotype_handle_tab(e.shiftKey ? 1 : 0);
      // Tab cycles focus inside C++; sync the JS-side cache so subsequent
      // Enter / caret-blink logic targets the right element, then
      // re-position the overlay onto whatever field Tab landed on.
      if (inst.exports.phenotype_get_focused_id)
        focusedId = inst.exports.phenotype_get_focused_id();
      syncHiddenInputForFocus();
      return;
    }
    if (e.key === 'Enter') {
      // Always read the fresh focused id — Tab updates it in C++ only,
      // so a stale local copy would dispatch to the wrong callback.
      const fid = inst.exports.phenotype_get_focused_id
        ? inst.exports.phenotype_get_focused_id()
        : focusedId;
      if (fid !== 0xFFFFFFFF) {
        e.preventDefault();
        if (inst.exports.phenotype_handle_event)
          inst.exports.phenotype_handle_event(fid);
        focusedId = fid;
      }
      return;
    }
  });

  // Caret blink timer
  setInterval(() => {
    if (focusedId !== 0xFFFFFFFF && inst.exports.phenotype_toggle_caret) {
      inst.exports.phenotype_toggle_caret();
    }
  }, 530);

  // Scroll
  canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    const maxScroll = Math.max(0, totalHeight - canvas.clientHeight);
    scrollY = Math.max(0, Math.min(scrollY + e.deltaY, maxScroll));
    if (inst.exports.phenotype_repaint) {
      inst.exports.phenotype_repaint(scrollY);
    }
    // Hit regions just shifted under a stationary pointer — re-derive
    // the cursor + hovered_id from the cached pointer position.
    applyHoverAt(lastClientX, lastClientY);
    // Keep the IME anchor following the focused field across the scroll.
    syncHiddenInputForFocus();
  }, { passive: false });

  // Resize
  let resizeRAF = 0;
  window.addEventListener('resize', () => {
    cancelAnimationFrame(resizeRAF);
    resizeRAF = requestAnimationFrame(() => {
      if (inst.exports.phenotype_repaint) {
        inst.exports.phenotype_repaint(scrollY);
        if (inst.exports.phenotype_get_total_height) {
          totalHeight = inst.exports.phenotype_get_total_height();
        }
      }
      applyHoverAt(lastClientX, lastClientY);
      syncHiddenInputForFocus();
    });
  });

  // Initial keyboard focus — make sure the OS IME has an editable
  // target ready before the user touches anything. Without this, the
  // first Korean keystroke after page load lands on document.body
  // instead of hiddenInput and gets dropped.
  hiddenInput.focus();
  } catch (e) {
    console.error('phenotype mount error:', e);
    rootElement.style.color = '#c00';
    rootElement.style.padding = '2rem';
    rootElement.style.fontFamily = 'monospace';
    rootElement.textContent = 'Error: ' + e.message;
  }
}
