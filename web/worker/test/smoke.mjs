// Black-box check of a running Worker (wrangler dev, or a deployment you can reach without Access):
//   node test/smoke.mjs http://127.0.0.1:8787
// Needs data published to the bucket it reads (see docs/web-port/HOSTING.md).

const base = (process.argv[2] || "http://127.0.0.1:8787").replace(/\/$/, "");
let failures = 0;

function check(name, ok, detail = "") {
  if (!ok) failures++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? "  " + detail : ""}`);
}

const ISOLATION = {
  "cross-origin-opener-policy": "same-origin",
  "cross-origin-embedder-policy": "require-corp",
  "cross-origin-resource-policy": "same-origin",
};

function isolated(res) {
  return Object.entries(ISOLATION).every(([k, v]) => res.headers.get(k) === v);
}

const get = (path, init) => fetch(base + path, { redirect: "manual", ...init });

const indexRes = await get("/data/index.json");
check("GET /data/index.json 200", indexRes.status === 200, String(indexRes.status));
check("  isolation headers", isolated(indexRes));
check("  content-type json", (indexRes.headers.get("content-type") || "").startsWith("application/json"));
check("  revalidates", indexRes.headers.get("cache-control") === "no-cache");
check("  has ETag", !!indexRes.headers.get("etag"));
const index = await indexRes.json();
const hash = index.files[0].chunks[0];
const size = Math.min(index.chunkSize, index.files[0].size);
const url = `/data/chunks/${hash}`;

const chunk = await get(url);
const body = new Uint8Array(await chunk.arrayBuffer());
check("GET chunk 200", chunk.status === 200 && body.length === size, `${chunk.status}, ${body.length} bytes`);
check("  isolation headers", isolated(chunk));
check("  immutable cache", chunk.headers.get("cache-control") === "public, max-age=31536000, immutable");
const digest = Buffer.from(await crypto.subtle.digest("SHA-256", body)).toString("hex");
check("  sha-256 matches key", digest === hash);
const etag = chunk.headers.get("etag");

const headRes = await get(url, { method: "HEAD" });
check("HEAD chunk", headRes.status === 200 && Number(headRes.headers.get("content-length")) === size && headRes.headers.get("etag") === etag);
check("  isolation headers", isolated(headRes));

const notModified = await get(url, { headers: { "If-None-Match": etag } });
check("If-None-Match -> 304", notModified.status === 304);
check("  isolation headers", isolated(notModified));
const headNotModified = await get(url, { method: "HEAD", headers: { "If-None-Match": etag } });
check("HEAD If-None-Match -> 304", headNotModified.status === 304);

const range = await get(url, { headers: { Range: "bytes=1-8" } });
check("Range 1-8 -> 206", range.status === 206 && (await range.arrayBuffer()).byteLength === 8
  && range.headers.get("content-range") === `bytes 1-8/${size}`);
check("  isolation headers", isolated(range));
const suffix = await get(url, { headers: { Range: "bytes=-4" } });
check("Range -4 -> 206", suffix.status === 206 && (await suffix.arrayBuffer()).byteLength === 4);
// Beyond the end: 416 whether or not the chunk is cached at the edge (pick one the page has not fetched).
const lastHash = index.files[index.files.length - 1].chunks.at(-1);
const beyond = await get(`/data/chunks/${lastHash}`, { headers: { Range: "bytes=999999999-" } });
check("Range beyond the end -> 416", beyond.status === 416 && /^bytes \*\/\d+$/.test(beyond.headers.get("content-range") || ""), String(beyond.status));
check("  isolation headers", isolated(beyond));

for (const path of ["/data/chunks/" + "0".repeat(64), "/data/", "/data", "/bin/", "/bin/missing.wasm", "/data/../etc/passwd", "/data/a%2F..%2Fb", "/nope"]) {
  const res = await get(path);
  check(`GET ${path} -> 404`, res.status === 404, String(res.status));
  check("  isolation headers", isolated(res));
}

const post = await get("/data/index.json", { method: "POST", body: "x" });
check("POST -> 405", post.status === 405 && post.headers.get("allow") === "GET, HEAD");
check("  isolation headers", isolated(post));

const page = await get("/");
check("GET / 200", page.status === 200 && (page.headers.get("content-type") || "").startsWith("text/html"));
check("  isolation headers", isolated(page));
const css = await get("/style.css");
check("GET /style.css", css.status === 200 && isolated(css));

console.log(failures === 0 ? "\nall passed" : `\n${failures} failed`);
process.exit(failures === 0 ? 0 : 1);
