// One Worker for the whole origin: the page comes from static assets, /data/* and /bin/* from an R2
// bucket, and every response carries the cross-origin isolation headers. The hostname sits behind
// Cloudflare Access, which runs before this code; nothing here authenticates anyone.

import {
  ISOLATION_HEADERS,
  cacheControlFor,
  contentTypeFor,
  etagMatches,
  isBucketNamespace,
  isEdgeCacheable,
  resolveBucketPath,
  resolveRange,
} from "./policy.js";

export default {
  async fetch(request, env, ctx) {
    let response;
    try {
      response = await handle(request, env, ctx);
    } catch (err) {
      console.error("unhandled", err && err.stack ? err.stack : String(err));
      response = text(500, "internal error");
    }
    return withIsolation(response);
  },
};

function withIsolation(response) {
  const headers = new Headers(response.headers);
  for (const [name, value] of Object.entries(ISOLATION_HEADERS)) headers.set(name, value);
  return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
}

function text(status, body, extra = {}) {
  return new Response(body + "\n", {
    status,
    headers: { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store", ...extra },
  });
}

async function handle(request, env, ctx) {
  const url = new URL(request.url);
  const target = resolveBucketPath(url.pathname);

  if (!target) {
    if (isBucketNamespace(url.pathname)) return text(404, "not found");
    return env.ASSETS.fetch(request); // the page; the assets binding does ETag, HEAD and 304 itself
  }
  if (request.method !== "GET" && request.method !== "HEAD") {
    return text(405, "method not allowed", { Allow: "GET, HEAD" });
  }
  return request.method === "HEAD" ? head(request, env, target) : get(request, env, ctx, target);
}

function objectHeaders(target, object) {
  const headers = new Headers();
  headers.set("Content-Type", contentTypeFor(target.key, object.httpMetadata && object.httpMetadata.contentType));
  headers.set("ETag", object.httpEtag);
  headers.set("Cache-Control", cacheControlFor(target.key, target.kind));
  headers.set("Last-Modified", object.uploaded.toUTCString());
  headers.set("Accept-Ranges", "bytes");
  return headers;
}

async function head(request, env, target) {
  const object = await env.DATA.head(target.key);
  if (!object) return text(404, "not found");
  const headers = objectHeaders(target, object);
  if (etagMatches(request.headers.get("If-None-Match"), object.httpEtag)) {
    return new Response(null, { status: 304, headers });
  }
  headers.set("Content-Length", String(object.size));
  return new Response(null, { status: 200, headers });
}

async function get(request, env, ctx, target) {
  const cacheable = isEdgeCacheable(target.key, target.kind);
  const cache = caches.default;

  if (cacheable) {
    // The Cache API answers Range and If-None-Match for a stored full response by itself.
    const hit = await cache.match(request);
    if (hit) return hit;
  }

  // R2 evaluates If-None-Match & co. and honours Range straight from the request headers.
  let object;
  try {
    object = await env.DATA.get(target.key, { onlyIf: request.headers, range: request.headers });
  } catch (err) {
    // R2 throws (code 10039) when the range starts beyond the object; malformed ranges it just ignores.
    if (request.headers.has("Range") && /10039|not satisfiable/i.test(String(err && err.message))) {
      const meta = await env.DATA.head(target.key);
      if (!meta) return text(404, "not found");
      return text(416, "range not satisfiable", { "Content-Range": `bytes */${meta.size}` });
    }
    throw err;
  }
  if (!object) return text(404, "not found");

  const headers = objectHeaders(target, object);
  if (!("body" in object)) {
    // A precondition failed: for If-None-Match / If-Modified-Since that means "unchanged".
    const unchanged = request.headers.has("If-None-Match") || request.headers.has("If-Modified-Since");
    return new Response(null, { status: unchanged ? 304 : 412, headers });
  }

  const range = request.headers.has("Range") ? resolveRange(object.range, object.size) : null;
  let status = 200;
  let length = object.size;
  if (range && (range.offset !== 0 || range.length !== object.size)) {
    status = 206;
    length = range.length;
    headers.set("Content-Range", `bytes ${range.offset}-${range.offset + range.length - 1}/${object.size}`);
  }
  headers.set("Content-Length", String(length));

  const response = new Response(object.body, { status, headers });
  if (cacheable && status === 200 && !request.headers.has("Range")) {
    ctx.waitUntil(cache.put(request, response.clone()));
  }
  return response;
}
