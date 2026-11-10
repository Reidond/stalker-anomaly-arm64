// JS library for the storage lab (linked with --js-library). Everything here runs on the I/O
// pthread: OPFS synchronous access handles exist only in dedicated workers, and each handle stays
// on the worker that opened it.
//
// IoShared layout (storage_lab.h; storage_lab.cpp static_asserts the offsets):
//   int32 req_seq 0, done_seq 4, op 8, chunk 12, dest 16, result 20, source 24, ready 28,
//   chunk_count 32, chunk_size 36, shared_view_ok 40, pad 44, uint32 crc[64] 48,
//   double fetch_ms 304, store_ms 312, read_ms 320, copy_ms 328, char error[256] 336.

addToLibrary({
  $labIo: {},

  lab_io_start__deps: ['$labIo', '$stringToUTF8'],
  lab_io_start: (io) => {
    const i32 = (off) => (io + off) >> 2;
    const f64 = (off) => (io + off) >> 3;
    const fail = (e) => {
      stringToUTF8(String(e && e.message || e), io + 336, 256);
      Atomics.store(HEAP32, i32(28), -1);
      Atomics.notify(HEAP32, i32(28));
    };

    const getChunk = async (index, dest) => {
      const L = labIo;
      const size = L.chunkSize;
      const at = index * size;
      if (L.present.has(index)) {
        const t0 = performance.now();
        let n;
        if (L.sharedOk) {
          n = L.store.read(HEAPU8.subarray(dest, dest + size), { at });
        } else {
          n = L.store.read(L.scratch, { at });
          HEAPU8.set(L.scratch.subarray(0, n), dest);
        }
        HEAPF64[f64(320)] = performance.now() - t0;
        HEAP32[i32(24)] = 2;
        return n;
      }
      const t0 = performance.now();
      const resp = await fetch(new URL(L.manifest.chunks[index].name, L.base));
      if (!resp.ok) throw new Error(`chunk ${index}: HTTP ${resp.status}`);
      const bytes = new Uint8Array(await resp.arrayBuffer());
      const t1 = performance.now();
      L.store.write(bytes, { at });
      L.store.flush();
      const t2 = performance.now();
      HEAPU8.set(bytes, dest);
      const t3 = performance.now();
      L.present.add(index);
      HEAPF64[f64(304)] = t1 - t0;
      HEAPF64[f64(312)] = t2 - t1;
      HEAPF64[f64(328)] = t3 - t2;
      HEAP32[i32(24)] = 1;
      return bytes.length;
    };

    const serve = (lastSeq) => {
      const wait = Atomics.waitAsync(HEAP32, i32(0), lastSeq);
      const go = () => handle(Atomics.load(HEAP32, i32(0)));
      if (wait.async) wait.value.then(go); else go();
    };

    const handle = async (seq) => {
      const op = HEAP32[i32(8)];
      let stop = false;
      try {
        if (op === 1) {
          HEAP32[i32(20)] = await getChunk(HEAP32[i32(12)], HEAP32[i32(16)]);
        } else if (op === 2) {
          _lab_io_run_bench();
          HEAP32[i32(20)] = 0;
        } else if (op === 3) {
          stop = true;
          try { labIo.store.close(); } catch (e) {}
          labIo.dir.removeEntry('chunks.bin').catch(() => {});
        }
      } catch (e) {
        HEAP32[i32(20)] = -1;
        stringToUTF8(String(e && e.message || e), io + 336, 256);
        console.error('[lab io]', e);
      }
      Atomics.store(HEAP32, i32(4), seq);
      Atomics.notify(HEAP32, i32(4));
      if (!stop) serve(seq);
    };

    (async () => {
      const root = await navigator.storage.getDirectory();
      const dir = await root.getDirectoryHandle('browser-lab', { create: true });
      const store = await (await dir.getFileHandle('chunks.bin', { create: true })).createSyncAccessHandle();
      store.truncate(0);
      const bench = await (await dir.getFileHandle('bench.bin', { create: true })).createSyncAccessHandle();
      bench.truncate(0);
      const base = new URL('chunks/', location.href);
      const manifest = await (await fetch(new URL('manifest.json', base))).json();

      // Can the handle read and write views of the shared wasm memory without a copy?
      let sharedOk = true;
      try {
        bench.write(HEAPU8.subarray(io, io + 16), { at: 0 });
        bench.read(HEAPU8.subarray(io + 336, io + 352), { at: 0 });
      } catch (e) {
        sharedOk = false;
        console.warn('[lab io] sync access handle rejects SharedArrayBuffer views:', e);
      }
      bench.truncate(0);
      HEAPU8.fill(0, io + 336, io + 352);

      Object.assign(labIo, {
        root, dir, store, bench, base, manifest, sharedOk,
        chunkSize: manifest.chunk_size,
        present: new Set(),
        scratch: sharedOk ? null : new Uint8Array(4 << 20),
      });
      HEAP32[i32(32)] = manifest.chunks.length;
      HEAP32[i32(36)] = manifest.chunk_size;
      HEAP32[i32(40)] = sharedOk ? 1 : 0;
      manifest.chunks.forEach((c, i) => { HEAPU32[i32(48) + i] = c.crc32 >>> 0; });
      Atomics.store(HEAP32, i32(28), 1);
      Atomics.notify(HEAP32, i32(28));
      serve(0);
    })().catch(fail);
  },

  lab_opfs_bench_open__deps: ['$labIo'],
  lab_opfs_bench_open: () => (labIo.bench ? 1 : 0),

  lab_opfs_write__deps: ['$labIo'],
  lab_opfs_write: (ptr, len, offset) => {
    if (labIo.sharedOk) return labIo.bench.write(HEAPU8.subarray(ptr, ptr + len), { at: offset });
    labIo.scratch.set(HEAPU8.subarray(ptr, ptr + len));
    return labIo.bench.write(labIo.scratch.subarray(0, len), { at: offset });
  },

  lab_opfs_read__deps: ['$labIo'],
  lab_opfs_read: (ptr, len, offset) => {
    if (labIo.sharedOk) return labIo.bench.read(HEAPU8.subarray(ptr, ptr + len), { at: offset });
    const n = labIo.bench.read(labIo.scratch.subarray(0, len), { at: offset });
    HEAPU8.set(labIo.scratch.subarray(0, n), ptr);
    return n;
  },

  lab_opfs_flush__deps: ['$labIo'],
  lab_opfs_flush: () => labIo.bench.flush(),

  lab_opfs_bench_close__deps: ['$labIo'],
  lab_opfs_bench_close: () => {
    try { labIo.bench.truncate(0); labIo.bench.close(); } catch (e) {}
    labIo.bench = null;
    labIo.dir.removeEntry('bench.bin').catch(() => {});
  },
});
