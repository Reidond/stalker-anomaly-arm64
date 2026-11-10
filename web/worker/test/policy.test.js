import test from "node:test";
import assert from "node:assert/strict";
import {
  IMMUTABLE,
  REVALIDATE,
  cacheControlFor,
  contentTypeFor,
  etagMatches,
  isBucketNamespace,
  isEdgeCacheable,
  resolveBucketPath,
  resolveRange,
} from "../src/policy.js";

const HASH = "ab".repeat(32);

test("resolveBucketPath accepts chunks, the index and binaries", () => {
  assert.deepEqual(resolveBucketPath(`/data/chunks/${HASH}`), { key: `data/chunks/${HASH}`, kind: "chunk" });
  assert.deepEqual(resolveBucketPath("/data/index.json"), { key: "data/index.json", kind: "data" });
  assert.deepEqual(resolveBucketPath("/bin/anomaly.wasm"), { key: "bin/anomaly.wasm", kind: "bin" });
});

test("resolveBucketPath rejects everything that is not a plain object name", () => {
  for (const p of ["/", "/index.html", "/data", "/data/", "/bin/", "/data//x", "/data/x/", "/data/../x",
    "/data/./x", "/data/a%2F..%2Fb", "/data/%2e%2e/x", "/data/a b", "/data/%zz", "/data/a\u0000b", "/datax/y"]) {
    assert.equal(resolveBucketPath(p), null, p);
  }
});

test("only a 64-digit lowercase hex name below data/chunks/ is a chunk", () => {
  assert.equal(resolveBucketPath("/data/chunks/abc").kind, "data");
  assert.equal(resolveBucketPath(`/data/chunks/${HASH.toUpperCase()}`).kind, "data");
});

test("isBucketNamespace", () => {
  for (const p of ["/data", "/data/", "/data/x", "/bin", "/bin/x"]) assert.equal(isBucketNamespace(p), true, p);
  for (const p of ["/", "/database", "/binary", "/x/data/"]) assert.equal(isBucketNamespace(p), false, p);
});

test("cache control: chunks and hashed binaries are immutable, the rest revalidates", () => {
  assert.equal(cacheControlFor(`data/chunks/${HASH}`, "chunk"), IMMUTABLE);
  assert.equal(cacheControlFor("data/index.json", "data"), REVALIDATE);
  assert.equal(cacheControlFor("bin/anomaly.wasm", "bin"), REVALIDATE);
  assert.equal(cacheControlFor("bin/anomaly.3fa9c0d27e1b.wasm", "bin"), IMMUTABLE);
  assert.equal(isEdgeCacheable(`data/chunks/${HASH}`, "chunk"), true);
  assert.equal(isEdgeCacheable("data/index.json", "data"), false);
});

test("content type follows the extension, wasm included", () => {
  assert.equal(contentTypeFor("bin/anomaly.wasm", "application/octet-stream"), "application/wasm");
  assert.equal(contentTypeFor("data/index.json", undefined), "application/json");
  assert.equal(contentTypeFor("bin/loader.js", undefined), "text/javascript");
  assert.equal(contentTypeFor(`data/chunks/${HASH}`, "text/plain"), "application/octet-stream");
  assert.equal(contentTypeFor("bin/blob.pak", "application/x-pak"), "application/x-pak");
  assert.equal(contentTypeFor("bin/blob.pak", undefined), "application/octet-stream");
});

test("etagMatches", () => {
  assert.equal(etagMatches(null, '"a"'), false);
  assert.equal(etagMatches('"a"', '"a"'), true);
  assert.equal(etagMatches('W/"a"', '"a"'), true);
  assert.equal(etagMatches('"b", "a"', '"a"'), true);
  assert.equal(etagMatches("*", '"a"'), true);
  assert.equal(etagMatches('"b"', '"a"'), false);
});

test("resolveRange", () => {
  assert.equal(resolveRange(undefined, 100), null);
  assert.deepEqual(resolveRange({ offset: 10, length: 5 }, 100), { offset: 10, length: 5 });
  assert.deepEqual(resolveRange({ offset: 90 }, 100), { offset: 90, length: 10 });
  assert.deepEqual(resolveRange({ offset: 90, length: 50 }, 100), { offset: 90, length: 10 });
  assert.deepEqual(resolveRange({ suffix: 4 }, 100), { offset: 96, length: 4 });
  assert.deepEqual(resolveRange({ suffix: 400 }, 100), { offset: 0, length: 100 });
});
