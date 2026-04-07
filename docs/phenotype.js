// phenotype.js — WASI polyfill + DOM command buffer executor
//
// Usage:
//   import { mount } from './phenotype.js';
//   mount('hello.wasm', document.body);

// --- DOM command opcodes (must match phenotype.cppm Cmd enum) ---

const CMD_CREATE_ELEMENT = 1;
const CMD_SET_TEXT = 2;
const CMD_APPEND_CHILD = 3;
const CMD_SET_ATTRIBUTE = 4;
const CMD_SET_STYLE = 5;
const CMD_SET_INNER_HTML = 6;
const CMD_ADD_CLASS = 7;
const CMD_REMOVE_CHILD = 8;
const CMD_CLEAR_CHILDREN = 9;
const CMD_REGISTER_CLICK = 10;

// --- DOM command buffer executor ---

function createExecutor(instance, rootElement) {
  const handles = new Map();
  handles.set(0, rootElement); // handle 0 = root element

  const decoder = new TextDecoder();
  function readString(bytes, offset, len) {
    return decoder.decode(bytes.slice(offset, offset + len));
  }

  function align4(pos) {
    return (pos + 3) & ~3;
  }

  return function flush() {
    const mem = instance.exports.memory.buffer;
    const bufOffset = instance.exports.phenotype_get_cmd_buf();
    const bufLen = instance.exports.phenotype_get_cmd_len();
    if (bufLen === 0) return;

    const bytes = new Uint8Array(mem);
    const view = new DataView(mem);
    let pos = bufOffset;
    const end = bufOffset + bufLen;

    while (pos < end) {
      const cmd = view.getUint32(pos, true); pos += 4;

      switch (cmd) {
        case CMD_CREATE_ELEMENT: {
          const handle = view.getUint32(pos, true); pos += 4;
          const tagLen = view.getUint32(pos, true); pos += 4;
          const tag = readString(bytes, pos, tagLen);
          pos = align4(pos + tagLen);
          handles.set(handle, document.createElement(tag));
          break;
        }
        case CMD_SET_TEXT: {
          const handle = view.getUint32(pos, true); pos += 4;
          const textLen = view.getUint32(pos, true); pos += 4;
          const text = readString(bytes, pos, textLen);
          pos = align4(pos + textLen);
          const el = handles.get(handle);
          if (el) el.textContent = text;
          break;
        }
        case CMD_APPEND_CHILD: {
          const parent = view.getUint32(pos, true); pos += 4;
          const child = view.getUint32(pos, true); pos += 4;
          const parentEl = handles.get(parent);
          const childEl = handles.get(child);
          if (parentEl && childEl) parentEl.appendChild(childEl);
          break;
        }
        case CMD_SET_ATTRIBUTE: {
          const handle = view.getUint32(pos, true); pos += 4;
          const keyLen = view.getUint32(pos, true); pos += 4;
          const key = readString(bytes, pos, keyLen);
          pos = align4(pos + keyLen);
          const valLen = view.getUint32(pos, true); pos += 4;
          const val = readString(bytes, pos, valLen);
          pos = align4(pos + valLen);
          const el = handles.get(handle);
          if (el) el.setAttribute(key, val);
          break;
        }
        case CMD_SET_STYLE: {
          const handle = view.getUint32(pos, true); pos += 4;
          const propLen = view.getUint32(pos, true); pos += 4;
          const prop = readString(bytes, pos, propLen);
          pos = align4(pos + propLen);
          const valLen = view.getUint32(pos, true); pos += 4;
          const val = readString(bytes, pos, valLen);
          pos = align4(pos + valLen);
          const el = handles.get(handle);
          if (el) el.style[prop] = val;
          break;
        }
        case CMD_SET_INNER_HTML: {
          const handle = view.getUint32(pos, true); pos += 4;
          const htmlLen = view.getUint32(pos, true); pos += 4;
          const html = readString(bytes, pos, htmlLen);
          pos = align4(pos + htmlLen);
          const el = handles.get(handle);
          if (el) el.innerHTML = html;
          break;
        }
        case CMD_ADD_CLASS: {
          const handle = view.getUint32(pos, true); pos += 4;
          const clsLen = view.getUint32(pos, true); pos += 4;
          const cls = readString(bytes, pos, clsLen);
          pos = align4(pos + clsLen);
          const el = handles.get(handle);
          if (el) el.classList.add(cls);
          break;
        }
        case CMD_REMOVE_CHILD: {
          const parent = view.getUint32(pos, true); pos += 4;
          const child = view.getUint32(pos, true); pos += 4;
          const parentEl = handles.get(parent);
          const childEl = handles.get(child);
          if (parentEl && childEl) parentEl.removeChild(childEl);
          break;
        }
        case CMD_CLEAR_CHILDREN: {
          const handle = view.getUint32(pos, true); pos += 4;
          const el = handles.get(handle);
          if (el) el.innerHTML = '';
          break;
        }
        case CMD_REGISTER_CLICK: {
          const handle = view.getUint32(pos, true); pos += 4;
          const callbackId = view.getUint32(pos, true); pos += 4;
          const el = handles.get(handle);
          if (el) {
            el.addEventListener('click', () => {
              instance.exports.phenotype_handle_event(callbackId);
            });
          }
          break;
        }
        default:
          console.error(`phenotype: unknown command ${cmd} at offset ${pos - 4}`);
          return;
      }
    }
  };
}

// --- Loader ---

export async function mount(wasmUrl, rootElement = document.body) {
  let inst = null;

  function getMemory() {
    return inst.exports.memory;
  }

  let executor = null;

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

  const phenotypeImports = {
    flush() {
      if (executor) executor();
    },
  };

  const result = await WebAssembly.instantiateStreaming(
    fetch(wasmUrl),
    {
      wasi_snapshot_preview1: wasiImports,
      phenotype: phenotypeImports,
    }
  );
  inst = result.instance;
  executor = createExecutor(inst, rootElement);

  if (inst.exports._start) {
    inst.exports._start();
  }
}
