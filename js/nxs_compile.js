/**
 * In-browser `.nxs` → `.nxb` compile (WASM) and fetch helper for DevTools-friendly
 * text/plain loads. Build WASM via: bash build_compile_wasm.sh
 */
import init, { compile_nxs, compile_nxs_columnar } from "./nxs_compile_wasm.js";
import { NxsReader } from "./nxs.js";

let wasmReady = null;

function ensureWasm() {
  if (!wasmReady) {
    wasmReady = init();
  }
  return wasmReady;
}

/**
 * Compile NXS source text to aligned `.nxb` bytes in memory.
 * @param {string} source
 * @returns {Promise<Uint8Array>}
 */
export async function compileNxsText(source) {
  await ensureWasm();
  return compile_nxs(source);
}

/**
 * Compile record blocks as columnar `.nxb` (layout forced; omit `@layout` pragma).
 * @param {string} source
 * @returns {Promise<Uint8Array>}
 */
export async function compileNxsColumnar(source) {
  await ensureWasm();
  if (typeof compile_nxs_columnar === "function") {
    return compile_nxs_columnar(source);
  }
  return compile_nxs(`@layout columnar\n${source}`);
}

/**
 * Fetch a `.nxs` URL as text (visible in Network → Response), compile, return reader.
 * @param {string} url
 * @returns {Promise<{ reader: NxsReader, buffer: ArrayBuffer, sourceText: string }>}
 */
export async function loadNxsDataset(url) {
  const res = await fetch(url);
  if (!res.ok) {
    throw new Error(`Failed to load ${url}: HTTP ${res.status}`);
  }
  const sourceText = await res.text();
  const compiled = await compileNxsText(sourceText);
  const buffer = compiled.buffer.slice(
    compiled.byteOffset,
    compiled.byteOffset + compiled.byteLength,
  );
  const reader = new NxsReader(buffer);
  return { reader, buffer, sourceText };
}

/** @deprecated Use compileNxsText */
export const NyxisCompiler = {
  compile: compileNxsText,
  createReader: (buffer) => new NxsReader(buffer),
};
