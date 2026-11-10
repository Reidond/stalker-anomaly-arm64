// Pure rules of the Worker: which paths are bucket reads, what they are called, how long they live in
// caches. No Workers APIs here, so `node --test` covers it.

export const ISOLATION_HEADERS = {
  // Cross-origin isolation, which SharedArrayBuffer and wasm threads need.
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
  // Nothing on this origin may be embedded by another origin.
  "Cross-Origin-Resource-Policy": "same-origin",
  "X-Content-Type-Options": "nosniff",
};

const KEY_PATTERN = /^[A-Za-z0-9._-]+(\/[A-Za-z0-9._-]+)*$/;
const CHUNK_PATTERN = /^data\/chunks\/[0-9a-f]{64}$/;
// A content hash between dots in the file name, as in app.3fa9c0d27e1b.wasm
const HASHED_NAME = /\.[0-9a-f]{12,64}\.[A-Za-z0-9]+$/;

export const IMMUTABLE = "public, max-age=31536000, immutable";
export const REVALIDATE = "no-cache";

/** True for /data, /bin and everything below them, whether or not the path is a valid object name. */
export function isBucketNamespace(pathname) {
  return /^\/(data|bin)(\/|$)/.test(pathname);
}

/** "/data/chunks/ab.." ->{ key: "data/chunks/ab..", kind: "chunk" }; null when the path is not a bucket read. */
export function resolveBucketPath(pathname) {
  if (!pathname.startsWith("/data/") && !pathname.startsWith("/bin/")) return null;
  let decoded;
  try {
    decoded = decodeURIComponent(pathname.slice(1));
  } catch {
    return null;
  }
  // Only plain path segments: no empty ones (so no trailing slash), no "." or "..", no odd characters.
  if (!KEY_PATTERN.test(decoded)) return null;
  if (decoded.split("/").some((s) => s === "." || s === "..")) return null;
  if (CHUNK_PATTERN.test(decoded)) return { key: decoded, kind: "chunk" };
  return { key: decoded, kind: decoded.startsWith("bin/") ? "bin" : "data" };
}

export function cacheControlFor(key, kind) {
  if (kind === "chunk") return IMMUTABLE;
  if (kind === "bin" && HASHED_NAME.test(key)) return IMMUTABLE;
  return REVALIDATE; // index.json and unhashed binaries: always ask, the ETag makes it cheap
}

/** Only immutable objects go to the edge cache; everything else would need purging after a publish. */
export function isEdgeCacheable(key, kind) {
  return cacheControlFor(key, kind) === IMMUTABLE;
}

const TYPES = {
  wasm: "application/wasm",
  json: "application/json",
  js: "text/javascript",
  mjs: "text/javascript",
  map: "application/json",
  html: "text/html; charset=utf-8",
  css: "text/css; charset=utf-8",
  txt: "text/plain; charset=utf-8",
};

/** The extension decides; the type stored with the object only matters for names without a known one. */
export function contentTypeFor(key, stored) {
  const base = key.slice(key.lastIndexOf("/") + 1);
  const dot = base.lastIndexOf(".");
  const ext = dot < 0 ? "" : base.slice(dot + 1).toLowerCase();
  if (TYPES[ext]) return TYPES[ext];
  if (key.startsWith("data/chunks/")) return "application/octet-stream";
  return stored || "application/octet-stream";
}

/** If-None-Match against one ETag, weak comparison as RFC 9110 prescribes for GET and HEAD. */
export function etagMatches(header, etag) {
  if (!header) return false;
  if (header.trim() === "*") return true;
  const strip = (v) => v.trim().replace(/^W\//, "");
  const want = strip(etag);
  return header.split(",").some((v) => strip(v) === want);
}

/** Absolute offset and length of a served range, from an R2 range descriptor and the object size. */
export function resolveRange(range, size) {
  if (!range) return null;
  let offset = range.offset;
  let length = range.length;
  if (range.suffix !== undefined) {
    length = Math.min(range.suffix, size);
    offset = size - length;
  } else {
    offset = offset ?? 0;
    length = Math.min(length ?? size - offset, size - offset);
  }
  return { offset, length };
}
