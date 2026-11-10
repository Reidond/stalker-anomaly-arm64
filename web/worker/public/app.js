// Hosting check: isolation flags, then index.json, then a few chunks verified against their hashes.

const INDEX_URL = "/data/index.json";
const SAMPLE_CHUNKS = 4;

const $ = (id) => document.getElementById(id);

function cell(text, cls) {
  const td = document.createElement("td");
  td.textContent = text;
  if (cls) td.className = cls;
  return td;
}

function addRow(name, ok, result, detail = "") {
  const tr = document.createElement("tr");
  tr.append(cell(name), cell(result, ok === true ? "ok" : ok === false ? "bad" : "warn"), cell(detail));
  $("results").tBodies[0].append(tr);
  return ok;
}

function fmtBytes(n) {
  return n >= 1048576 ? (n / 1048576).toFixed(1) + " MiB" : n >= 1024 ? (n / 1024).toFixed(1) + " KiB" : n + " B";
}

function hex(buffer) {
  return [...new Uint8Array(buffer)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function showEnvironment() {
  const sab = typeof SharedArrayBuffer !== "undefined";
  const rows = [
    ["crossOriginIsolated", String(window.crossOriginIsolated), window.crossOriginIsolated],
    ["SharedArrayBuffer", sab ? "available" : "missing", sab],
    ["secure context", String(window.isSecureContext), window.isSecureContext],
    ["WebGPU", "gpu" in navigator ? "navigator.gpu present" : "missing", "gpu" in navigator],
    ["WebHID", "hid" in navigator ? "navigator.hid present" : "missing", "hid" in navigator],
    ["OPFS", navigator.storage && navigator.storage.getDirectory ? "available" : "missing",
      !!(navigator.storage && navigator.storage.getDirectory)],
    ["hardwareConcurrency", String(navigator.hardwareConcurrency), null],
  ];
  const dl = $("env");
  dl.replaceChildren();
  for (const [k, v, ok] of rows) {
    const dt = document.createElement("dt");
    dt.textContent = k;
    const dd = document.createElement("dd");
    dd.textContent = v;
    if (ok !== null) dd.className = ok ? "ok" : "bad";
    dl.append(dt, dd);
  }
}

async function timed(fn) {
  const t0 = performance.now();
  const value = await fn();
  return [value, performance.now() - t0];
}

function headerSummary(res) {
  const pick = ["cross-origin-opener-policy", "cross-origin-embedder-policy", "cross-origin-resource-policy"];
  const missing = pick.filter((h) => !res.headers.get(h));
  return { ok: missing.length === 0, missing };
}

async function run() {
  $("results").tBodies[0].replaceChildren();
  $("results").hidden = false;
  $("rerun").hidden = true;
  $("summary").textContent = "Running…";
  let failures = 0;
  const check = (ok) => { if (ok === false) failures++; return ok; };

  try {
    const [res, ms] = await timed(() => fetch(INDEX_URL, { cache: "no-store" }));
    check(addRow("GET index.json", res.ok, `${res.status}`, `${ms.toFixed(0)} ms, ETag ${res.headers.get("etag")}`));
    if (!res.ok) throw new Error("no index");
    const hdr = headerSummary(res);
    check(addRow("isolation headers on data", hdr.ok, hdr.ok ? "present" : "missing", hdr.missing.join(", ")));
    check(addRow("content type", /^application\/json/.test(res.headers.get("content-type") || ""),
      res.headers.get("content-type") || "none"));

    const index = await res.json();
    const chunkCount = index.files.reduce((n, f) => n + f.chunks.length, 0);
    const archives = index.files.filter((f) => f.archive);
    const entries = archives.reduce((n, f) => n + f.archive.entries.length, 0);
    check(addRow("index", index.format === "anomaly-web-index/1", index.format,
      `${index.files.length} files, ${fmtBytes(index.totalSize)}, ${chunkCount} chunks of ${fmtBytes(index.chunkSize)}, ` +
      `${archives.length} archives with ${entries} entries`));

    const distinct = [];
    const seen = new Set();
    for (const f of index.files) {
      for (const [i, h] of f.chunks.entries()) {
        if (!seen.has(h)) {
          seen.add(h);
          distinct.push({ hash: h, size: Math.min(index.chunkSize, f.size - i * index.chunkSize), file: f.path });
        }
      }
    }
    // Smallest first, so the check stays quick on a big data set.
    distinct.sort((a, b) => a.size - b.size);

    let first = null;
    for (const c of distinct.slice(0, SAMPLE_CHUNKS)) {
      const url = `/data/${index.chunkPath}${c.hash}`;
      const [r, t] = await timed(() => fetch(url, { cache: "no-store" }));
      if (!r.ok) {
        check(addRow(`GET chunk ${c.hash.slice(0, 10)}`, false, `${r.status}`, c.file));
        continue;
      }
      const buf = await r.arrayBuffer();
      const got = hex(await crypto.subtle.digest("SHA-256", buf));
      const good = got === c.hash && buf.byteLength === c.size;
      check(addRow(`chunk ${c.hash.slice(0, 10)}`, good, good ? "hash verified" : "HASH MISMATCH",
        `${fmtBytes(buf.byteLength)} in ${t.toFixed(0)} ms, ${c.file}, cf-cache-status ${r.headers.get("cf-cache-status") || "n/a"}, ` +
        `cache-control ${r.headers.get("cache-control")}`));
      first = first || { url, etag: r.headers.get("etag"), size: buf.byteLength };
    }

    if (first) {
      const h = await fetch(first.url, { method: "HEAD", cache: "no-store" });
      check(addRow("HEAD chunk", h.ok && Number(h.headers.get("content-length")) === first.size && h.headers.get("etag") === first.etag,
        `${h.status}`, `content-length ${h.headers.get("content-length")}, ETag ${h.headers.get("etag")}`));

      const c = await fetch(first.url, { headers: { "If-None-Match": first.etag }, cache: "no-store" });
      check(addRow("If-None-Match", c.status === 304, `${c.status}`, "expected 304"));

      const rg = await fetch(first.url, { headers: { Range: "bytes=0-15" }, cache: "no-store" });
      const rb = await rg.arrayBuffer();
      check(addRow("Range 0-15", rg.status === 206 && rb.byteLength === Math.min(16, first.size), `${rg.status}`,
        `${rb.byteLength} bytes, ${rg.headers.get("content-range")}`));
    }

    const miss = await fetch("/data/chunks/" + "0".repeat(64), { cache: "no-store" });
    check(addRow("missing chunk", miss.status === 404, `${miss.status}`, "expected 404"));
  } catch (err) {
    check(addRow("error", false, String(err && err.message || err)));
  }

  $("summary").textContent = failures === 0 ? "All checks passed." : `${failures} check(s) failed.`;
  $("summary").className = failures === 0 ? "ok" : "bad";
  $("rerun").hidden = false;
}

showEnvironment();
$("rerun").addEventListener("click", run);
run();
