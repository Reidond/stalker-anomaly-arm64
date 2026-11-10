// Page main thread of the browser lab: loads the wasm program (which runs on a worker pthread),
// owns the DOM, the input bridge, the blocked-worker probe, storage persistence and WebHID.
import {
  DualSense, TriggerType, FEEDBACK_FLAGS, PadButton, buildOutputReport, emptyState, parseInputReport,
  readFeedbackShared, writePadShared,
} from './dualsense.js';
import { runSelfTest, syntheticUsbPayload } from './dualsense_selftest.js';

const $ = (id) => document.getElementById(id);
const epochNow = () => performance.timeOrigin + performance.now();
const fmt = (v, d = 1) => (typeof v === 'number' && Number.isFinite(v) ? v.toFixed(d) : String(v));

// ---- Results ------------------------------------------------------------------------------------

const ROWS = [
  ['env.isolation', 1, 'Cross-origin isolation, SharedArrayBuffer, wasm threads'],
  ['thread.app', 1, 'App code on a worker pthread (PROXY_TO_PTHREAD)'],
  ['gpu.worker', 1, 'WebGPU device on the worker, OffscreenCanvas surface'],
  ['gpu.adapter', 1, 'Adapter features and limits'],
  ['gpu.mrt', 1, 'Pass with 4 render targets, composite to the canvas'],
  ['gpu.bc', 1, 'BC1 and BC3 textures'],
  ['gpu.occlusion', 1, 'Occlusion query: frames until readable, count or boolean'],
  ['gpu.pipeline_async', 1, 'createRenderPipelineAsync'],
  ['gpu.upload', 1, 'Dynamic buffers: thousands of draws, batched writeBuffer'],
  ['gpu.block', 1, 'Presenting while the worker is blocked'],
  ['input.sdl_init', 2, 'SDL3 video initialised on the worker'],
  ['input.sdl_keys', 2, 'Keyboard events through SDL3'],
  ['input.sdl_mouse', 2, 'Mouse buttons, motion and wheel through SDL3'],
  ['input.sdl_relative', 2, 'SDL3 relative mouse mode gives pointer lock'],
  ['input.bridge_keys', 2, 'Keyboard events through the bridge'],
  ['input.bridge_mouse', 2, 'Mouse events through the bridge'],
  ['input.bridge_lock', 2, 'Pointer lock requested by the page'],
  ['input.prevent_default', 2, 'Browser shortcuts suppressed (preventDefault)'],
  ['input.fullscreen', 2, 'Fullscreen'],
  ['input.keyboard_lock', 2, 'Keyboard Lock keeps Esc in fullscreen'],
  ['input.lock_loss', 2, 'What the app sees when the browser drops pointer lock'],
  ['storage.persist', 3, 'navigator.storage.persist()'],
  ['storage.estimate', 3, 'Storage quota estimate'],
  ['storage.blocking', 3, 'Engine thread blocked while the I/O thread fetches and stores a chunk'],
  ['storage.opfs_bench', 3, 'OPFS sync access handle from a wasm thread, 1 GiB in 4 MiB chunks'],
  ['pad.webhid', 4, 'WebHID available'],
  ['pad.selftest', 4, 'Report parser and encoder self-test (no hardware)'],
  ['pad.shared', 4, 'PadShared to the worker’s PadSnapshot (synthetic reports)'],
  ['pad.device', 4, 'A real DualSense over USB and Bluetooth (manual checklist)'],
];

const results = new Map();
const rowEls = new Map();

function buildTable() {
  const tbody = $('results').querySelector('tbody');
  for (const [id, item, title] of ROWS) {
    const tr = document.createElement('tr');
    tr.innerHTML = `<td class="item">${item}</td><td class="check"></td><td class="verdict"></td><td class="result"></td>`;
    tr.querySelector('.check').textContent = title;
    tr.title = id;
    tbody.appendChild(tr);
    rowEls.set(id, tr);
    setResult(id, 'pending', 'Not run yet.');
  }
}

function setResult(id, status, summary, data) {
  results.set(id, { id, status, summary, data: data ?? null, at: new Date().toISOString() });
  let tr = rowEls.get(id);
  if (!tr) {
    tr = document.createElement('tr');
    tr.innerHTML = '<td class="item">–</td><td class="check"></td><td class="verdict"></td><td class="result"></td>';
    tr.querySelector('.check').textContent = id;
    $('results').querySelector('tbody').appendChild(tr);
    rowEls.set(id, tr);
  }
  tr.querySelector('.verdict').innerHTML = `<span class="badge ${status}">${status}</span>`;
  const cell = tr.querySelector('.result');
  cell.textContent = summary;
  if (data && Object.keys(data).length) {
    const det = document.createElement('details');
    det.innerHTML = '<summary>data</summary><pre></pre>';
    det.querySelector('pre').textContent = JSON.stringify(data, null, 1);
    cell.appendChild(det);
  }
  if (status !== 'pending') console.log(`[lab] ${id} ${status}: ${summary}`);
}

function log(text) {
  const el = $('log');
  el.textContent += `${new Date().toISOString().slice(11, 23)}  ${text}\n`;
  el.scrollTop = el.scrollHeight;
  console.log('[lab log]', text);
}

// Hooks called from wasm (MAIN_THREAD_ASYNC_EM_ASM).
globalThis.labLog = log;
globalThis.labReport = (r) => {
  if (r.id === 'input.live') return onLive(r.data);
  if (r.id === 'input.lock_loss') return onWorkerLockLoss(r);
  if (r.id === 'input.sdl_init' && r.status === 'pass') installPreventDefaultProbe();
  if (r.id === 'storage.opfs_bench') setTimeout(estimate, 1500);
  setResult(r.id, r.status, r.summary, r.data);
};

// ---- Environment ---------------------------------------------------------------------------------

function environment() {
  const ua = navigator.userAgentData?.brands?.filter((b) => !/Not.?A.?Brand/i.test(b.brand))
    .map((b) => `${b.brand} ${b.version}`).join(', ') || navigator.userAgent;
  const checks = [
    ['crossOriginIsolated', globalThis.crossOriginIsolated],
    ['SharedArrayBuffer', typeof SharedArrayBuffer === 'function'],
    ['WebGPU', 'gpu' in navigator],
    ['OffscreenCanvas', typeof HTMLCanvasElement.prototype.transferControlToOffscreen === 'function'],
    ['OPFS', !!navigator.storage?.getDirectory],
    ['WebHID', 'hid' in navigator],
    ['Keyboard Lock', !!navigator.keyboard?.lock],
    ['Atomics.waitAsync', typeof Atomics.waitAsync === 'function'],
  ];
  $('env').innerHTML = '';
  const add = (text, cls) => { const s = document.createElement('span'); s.textContent = text; if (cls) s.className = cls; $('env').appendChild(s); };
  add(ua);
  add(`${navigator.hardwareConcurrency} hardware threads`);
  for (const [name, ok] of checks) add(`${ok ? '✓' : '✗'} ${name}`, ok ? 'ok' : 'no');
  const iso = globalThis.crossOriginIsolated && typeof SharedArrayBuffer === 'function';
  setResult('env.isolation', iso ? 'pass' : 'fail',
    iso ? `crossOriginIsolated with COOP/COEP; ${navigator.hardwareConcurrency} hardware threads; ${ua}`
      : 'Not cross-origin isolated: serve with web/lab/serve.py (COOP same-origin, COEP require-corp).',
    Object.fromEntries([...checks, ['userAgent', navigator.userAgent], ['hardwareConcurrency', navigator.hardwareConcurrency]]));
  setResult('pad.webhid', 'hid' in navigator ? 'pass' : 'fail',
    'hid' in navigator ? 'navigator.hid present (Chrome/Edge desktop, secure context)' : 'navigator.hid missing: this browser has no WebHID');
  return iso;
}

// ---- Wasm module ---------------------------------------------------------------------------------

const canvas = $('canvas');
let wasmReady = false;

function command(cmd, arg = 0) {
  if (!wasmReady) return log(`command ${cmd} ignored: wasm not running`);
  Module._lab_command(cmd, arg);
}

function loadWasm() {
  globalThis.Module = {
    canvas,
    print: (t) => log(t),
    printErr: (t) => log(`stderr: ${t}`),
    onRuntimeInitialized: () => { wasmReady = true; },
    onAbort: (what) => setResult('lab.abort', 'fail', `wasm aborted: ${what}`),
  };
  const s = document.createElement('script');
  s.src = 'lab_wasm.js';
  s.onerror = () => setResult('lab.abort', 'fail', 'lab_wasm.js failed to load; run web/lab/build.sh');
  document.body.appendChild(s);
}

const heap = () => Module.HEAPU8.buffer;

// ---- Input bridge --------------------------------------------------------------------------------

const KEY_CODES = [];
const keyIndex = (code) => { let i = KEY_CODES.indexOf(code); if (i < 0) { i = KEY_CODES.length; KEY_CODES.push(code); } return i; };
const GAME_KEYS = new Set(['Tab', 'Space', 'Backspace', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Quote', 'Slash',
  'F1', 'F2', 'F3', 'F4', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11']);
const B = { keydown: 1, keyup: 2, pointerdown: 3, pointerup: 4, pointermove: 5, wheel: 6, lock: 7, fullscreen: 8, kblock: 9, blur: 10, focus: 11, visibility: 12 };
let bridge = null; // {ring, cap, pad, feedback}

globalThis.labAttach = (ring, cap, pad, feedback) => {
  bridge = { ring, cap, pad, feedback };
  log(`bridge ring at ${ring} (${cap} slots), PadShared at ${pad}, PadFeedbackShared at ${feedback}`);
  setTimeout(runSyntheticPad, 1500);
};

function push(type, code = 0, buttons = 0, extra = 0, dx = 0, dy = 0, epoch = epochNow()) {
  if (!bridge) return;
  const buf = heap();
  const i32 = new Int32Array(buf), f32 = new Float32Array(buf), f64 = new Float64Array(buf);
  const h = bridge.ring >> 2;
  const w = Atomics.load(i32, h), r = Atomics.load(i32, h + 1);
  if (w - r >= bridge.cap) { Atomics.add(i32, h + 3, 1); return; }
  const slot = bridge.ring + 16 + (w % bridge.cap) * 32;
  i32[slot >> 2] = type;
  i32[(slot >> 2) + 1] = code;
  i32[(slot >> 2) + 2] = buttons;
  i32[(slot >> 2) + 3] = extra;
  f32[(slot >> 2) + 4] = dx;
  f32[(slot >> 2) + 5] = dy;
  f64[(slot + 24) >> 3] = epoch;
  Atomics.store(i32, h, w + 1);
}

const page = {
  lockRequest: null, // {source, t}
  lockSource: null,
  lockedAt: 0,
  kbLock: null, // 'active' | error text
  escWhileLocked: 0,
  fullscreenSource: null,
  keyChecks: { sdlTested: 0, sdlPrevented: 0, bridgeTested: 0, bridgePrevented: 0 },
  lossPage: null,
};

const modifiers = (e) => (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0);

function installBridge() {
  document.addEventListener('keydown', (e) => {
    if (e.repeat) return;
    push(B.keydown, keyIndex(e.code), modifiers(e), 0, 0, 0, performance.timeOrigin + e.timeStamp);
    if (e.code === 'KeyL') page.lockRequest = { source: 'sdl', how: 'L key on the worker', t: performance.now() };
    if (e.code === 'KeyF') page.fullscreenRequest = { source: 'sdl', t: performance.now() };
    if (e.code === 'KeyU') page.appUnlock = performance.now();
    if (e.code === 'Escape' && document.fullscreenElement && page.kbLock === 'active') page.escWhileLocked++;
    if ($('bridge-capture').checked && GAME_KEYS.has(e.code)) {
      e.preventDefault();
      e.labBridgePrevented = true;
    }
  });
  document.addEventListener('keyup', (e) => push(B.keyup, keyIndex(e.code), modifiers(e), 0, 0, 0, performance.timeOrigin + e.timeStamp));
  canvas.addEventListener('pointerdown', (e) => { canvas.focus(); push(B.pointerdown, 0, e.buttons, e.button, 0, 0, performance.timeOrigin + e.timeStamp); });
  canvas.addEventListener('pointerup', (e) => push(B.pointerup, 0, e.buttons, e.button, 0, 0, performance.timeOrigin + e.timeStamp));
  canvas.addEventListener('pointermove', (e) => push(B.pointermove, 0, e.buttons, 0, e.movementX, e.movementY, performance.timeOrigin + e.timeStamp));
  canvas.addEventListener('wheel', (e) => {
    push(B.wheel, 0, 0, e.deltaMode, e.deltaX, e.deltaY, performance.timeOrigin + e.timeStamp);
    if ($('bridge-capture').checked) e.preventDefault();
  }, { passive: false });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  document.addEventListener('pointerlockchange', onPointerLockChange);
  document.addEventListener('pointerlockerror', () => log('pointerlockerror'));
  document.addEventListener('fullscreenchange', onFullscreenChange);
  window.addEventListener('blur', () => push(B.blur));
  window.addEventListener('focus', () => push(B.focus));
  document.addEventListener('visibilitychange', () => push(B.visibility, document.visibilityState === 'visible' ? 1 : 0));
}

// Registered after SDL's own window keydown listener, so it sees what SDL did to the event.
function installPreventDefaultProbe() {
  window.addEventListener('keydown', (e) => {
    if (e.repeat || !GAME_KEYS.has(e.code)) return;
    const k = page.keyChecks;
    if (e.labBridgePrevented) {
      k.bridgeTested++;
      k.bridgePrevented++;
    } else {
      k.sdlTested++;
      if (e.defaultPrevented) k.sdlPrevented++;
    }
    const sdlPart = k.sdlTested ? `SDL3 on the worker prevented ${k.sdlPrevented} of ${k.sdlTested} presses of browser keys (bridge suppression off)` : 'SDL3 not measured yet: untick "Suppress browser keys" and press Tab or Space';
    const bridgePart = k.bridgeTested ? `; the bridge prevented ${k.bridgePrevented} of ${k.bridgeTested}` : '';
    const status = k.sdlTested && k.bridgeTested ? (k.bridgePrevented === k.bridgeTested ? 'pass' : 'fail') : 'running';
    setResult('input.prevent_default', status, sdlPart + bridgePart, { ...k, keys: [...GAME_KEYS] });
  });
}

function onPointerLockChange() {
  const locked = document.pointerLockElement === canvas;
  push(B.lock, locked ? 1 : 0);
  const now = performance.now();
  if (locked) {
    const req = page.lockRequest && now - page.lockRequest.t < 10000 ? page.lockRequest : { source: 'sdl', how: 'SDL re-grab on mouse click', t: now };
    page.lockSource = req;
    page.lockedAt = now;
    page.lockStartSdl = lastLive ? { ...lastLive.sdl } : null;
    page.lockStartBridge = lastLive ? { ...lastLive.bridge } : null;
    log(`pointer lock acquired (${req.source}: ${req.how}) after ${fmt(now - req.t, 0)} ms`);
    const prev = results.get('input.sdl_relative');
    if (req.source === 'page')
      setResult('input.bridge_lock', 'pass', `requestPointerLock() in the page's click handler locked the canvas in ${fmt(now - req.t, 0)} ms`, { latency_ms: now - req.t });
    else if (prev?.status === 'pass')
      setResult('input.sdl_relative', 'pass', `${prev.summary.replace(/; re-grabbed.*$/, '')}; re-grabbed by ${req.how} after the lock was dropped`, { ...prev.data, regrab: req.how });
    else
      setResult('input.sdl_relative', 'running', `pointer lock acquired via ${req.how} after ${fmt(now - req.t, 0)} ms; waiting for relative motion`, { latency_ms: now - req.t, how: req.how });
    page.lockRequest = null;
  } else if (page.lockSource) {
    page.lossPage = { heldMs: now - page.lockedAt, source: page.lockSource.source, appInitiated: !!page.appUnlock && now - page.appUnlock < 500, fullscreen: !!document.fullscreenElement, keyboardLock: page.kbLock };
    log(`pointer lock lost after ${fmt(page.lossPage.heldMs, 0)} ms`);
    if (!results.get('input.lock_loss') || results.get('input.lock_loss').status !== 'pass')
      setResult('input.lock_loss', 'running', `page received pointerlockchange (unlocked) after ${fmt(page.lossPage.heldMs, 0)} ms; waiting for the worker`, page.lossPage);
    page.lockSource = null;
  }
}

// The interesting loss is one SDL did not ask for while it held the lock in relative mode.
function onWorkerLockLoss(r) {
  const p = page.lossPage;
  if (!p || p.source !== 'sdl' || !r.data.sdl_relative_after_loss) {
    log(`lock loss (source ${p?.source}, app-initiated or SDL relative off): ${r.summary}`);
    return;
  }
  const events = r.data.sdl_events_after_loss || 'none';
  setResult('input.lock_loss', 'pass',
    `The page gets a pointerlockchange event; SDL3 on the worker gets no event of its own for it (events in the next second: ${events}) and keeps relative mode on, so the game would not notice. The engine needs the page's event (bridge) to open its menu.`,
    { page: p, worker: r.data });
}

function onFullscreenChange() {
  const el = document.fullscreenElement;
  push(B.fullscreen, el ? 1 : 0);
  if (el) {
    const src = page.fullscreenRequest && performance.now() - page.fullscreenRequest.t < 10000 ? page.fullscreenRequest.source : 'unknown';
    page.fullscreenSource = src;
    log(`fullscreen entered on #${el.id} (${src})`);
    const prev = results.get('input.fullscreen');
    const both = prev?.data?.sources ? new Set(prev.data.sources) : new Set();
    both.add(`${src}:#${el.id}`);
    setResult('input.fullscreen', 'pass', `fullscreen entered: ${[...both].join(', ')}`, { sources: [...both] });
  } else {
    log('fullscreen exited');
    if (page.kbLock === 'active') {
      const esc = page.escWhileLocked;
      setResult('input.keyboard_lock', esc > 0 ? 'pass' : 'unverified',
        esc > 0 ? `navigator.keyboard.lock(['Escape']) held: ${esc} Esc press(es) reached the page while fullscreen stayed on`
          : 'keyboard.lock resolved, but no Esc press was seen in fullscreen; press Esc once, then hold Esc to leave',
        { escPressesInFullscreen: esc });
      navigator.keyboard?.unlock();
      page.kbLock = null;
    }
  }
}

// ---- Live input panel ----------------------------------------------------------------------------

let lastLive = null;
let lastLiveAt = 0; // the worker reports every 250 ms while it is iterating

function onLive(d) {
  lastLive = d;
  lastLiveAt = performance.now();
  const lat = (s) => (s && s.count ? `${fmt(s.median)} ms median, ${fmt(s.p95)} p95 (${s.count})` : 'n/a');
  const card = (title, body) => `<div class="card"><h3>${title}</h3><div class="num"></div></div>`.replace('<div class="num"></div>', `<div class="num">${body}</div>`);
  const counters = (c) => `keys ${c.key_down} down / ${c.key_up} up, last ${c.last_key.startsWith('#') ? KEY_CODES[+c.last_key.slice(1)] ?? c.last_key : c.last_key || '–'}\n` +
    `buttons ${c.button_down} down / ${c.button_up} up\nmotion ${c.motion} events, Δ ${fmt(c.dx, 0)}, ${fmt(c.dy, 0)}\nwheel ${c.wheel}`;
  const pad = d.pad;
  const padText = pad.reports ? `reports ${pad.reports}, buttons 0x${(pad.buttons >>> 0).toString(16)}\nL ${fmt(pad.left[0], 2)}, ${fmt(pad.left[1], 2)}  R ${fmt(pad.right[0], 2)}, ${fmt(pad.right[1], 2)}\nL2 ${fmt(pad.triggers[0], 2)} R2 ${fmt(pad.triggers[1], 2)}  battery ${fmt(pad.battery, 2)}` : 'no PadShared writes yet';
  $('live').innerHTML = [
    card('SDL3 on the worker', `${counters(d.sdl)}\nrelative mode ${d.sdl_relative ? 'on' : 'off'}, fullscreen flag ${d.sdl_fullscreen_flag ? 'on' : 'off'}\nkey latency ${lat(d.sdl_key_latency_ms)}\nbutton latency ${lat(d.sdl_button_latency_ms)}`),
    card('Page bridge (shared ring)', `${counters(d.bridge)}\npointer lock ${document.pointerLockElement === canvas ? 'on' : 'off'}, fullscreen ${document.fullscreenElement ? 'on' : 'off'}, dropped ${d.bridge_dropped}\nlatency ${lat(d.bridge_latency_ms)}`),
    card('Worker’s PadSnapshot', padText),
  ].join('');

  const s = d.sdl, b = d.bridge;
  if (s.key_down > 0)
    setResult('input.sdl_keys', 'pass', `${s.key_down} key presses reached SDL3 on the worker (bridge: ${b.key_down}); page event to SDL event ${lat(d.sdl_key_latency_ms)}`, { sdl: s, bridge_key_down: b.key_down, latency: d.sdl_key_latency_ms, event_types: d.sdl_event_types });
  if (s.button_down > 0 && s.motion > 0)
    setResult('input.sdl_mouse', s.wheel > 0 ? 'pass' : 'running', `SDL3: ${s.button_down} button presses, ${s.motion} motion events, ${s.wheel} wheel events; page event to SDL event ${lat(d.sdl_button_latency_ms)}${s.wheel > 0 ? '' : ' (scroll over the canvas to finish)'}`, { sdl: s, latency: d.sdl_button_latency_ms });
  if (b.key_down > 0)
    setResult('input.bridge_keys', 'pass', `${b.key_down} key presses through the ring; page event to worker ${lat(d.bridge_latency_ms)}; ${d.bridge_dropped} dropped`, { bridge: b, latency: d.bridge_latency_ms });
  if (b.button_down > 0 && b.motion > 0)
    setResult('input.bridge_mouse', b.wheel > 0 ? 'pass' : 'running', `bridge: ${b.button_down} button presses, ${b.motion} motion events, ${b.wheel} wheel events`, { bridge: b });

  // Relative motion while SDL holds the lock.
  if (page.lockSource && page.lockSource.source === 'sdl' && page.lockStartSdl) {
    const ds = { n: s.motion - page.lockStartSdl.motion, dx: s.dx - page.lockStartSdl.dx, dy: s.dy - page.lockStartSdl.dy };
    const db = { n: b.motion - page.lockStartBridge.motion, dx: b.dx - page.lockStartBridge.dx, dy: b.dy - page.lockStartBridge.dy };
    if (ds.n > 5 && d.sdl_relative)
      setResult('input.sdl_relative', 'pass', `SDL_SetWindowRelativeMouseMode on the worker → pointer lock (${page.lockSource.how}); ${ds.n} relative motion events, Δ ${fmt(ds.dx, 0)}, ${fmt(ds.dy, 0)} (bridge movementX/Y Δ ${fmt(db.dx, 0)}, ${fmt(db.dy, 0)})`, { sdl: ds, bridge: db, how: page.lockSource.how });
  }

  if (pendingPadCheck && pad.reports >= pendingPadCheck.reports) checkSyntheticPad(pad);
}

// ---- Buttons -------------------------------------------------------------------------------------

function installControls() {
  for (const b of document.querySelectorAll('[data-cmd]')) {
    b.addEventListener('click', () => {
      const cmd = Number(b.dataset.cmd);
      if (cmd === 1) page.lockRequest = { source: 'sdl', how: 'button → SDL_SetWindowRelativeMouseMode on the worker', t: performance.now() };
      if (cmd === 2) page.appUnlock = performance.now();
      if (cmd === 3) page.fullscreenRequest = { source: 'sdl', t: performance.now() };
      command(cmd, Number(b.dataset.arg || 0));
    });
  }
  $('page-lock').addEventListener('click', async () => {
    page.lockRequest = { source: 'page', how: 'canvas.requestPointerLock() in a click handler', t: performance.now() };
    try {
      await canvas.requestPointerLock({ unadjustedMovement: true });
    } catch (e) {
      log(`requestPointerLock({unadjustedMovement}) failed (${e.message}); retrying without it`);
      try { await canvas.requestPointerLock(); } catch (e2) { setResult('input.bridge_lock', 'fail', `requestPointerLock failed: ${e2.message}`); }
    }
  });
  $('page-fullscreen').addEventListener('click', async () => {
    page.fullscreenRequest = { source: 'page', t: performance.now() };
    try {
      await $('stage').requestFullscreen({ navigationUI: 'hide' });
    } catch (e) {
      return setResult('input.fullscreen', 'fail', `requestFullscreen failed: ${e.message}`);
    }
    if (!navigator.keyboard?.lock) {
      page.kbLock = 'unsupported';
      push(B.kblock, 0);
      return setResult('input.keyboard_lock', 'fail', 'navigator.keyboard.lock is not available in this browser');
    }
    try {
      await navigator.keyboard.lock(['Escape']);
      page.kbLock = 'active';
      page.escWhileLocked = 0;
      push(B.kblock, 1);
      setResult('input.keyboard_lock', 'running', "keyboard.lock(['Escape']) resolved; press Esc (stays fullscreen), then hold Esc to leave", {});
    } catch (e) {
      page.kbLock = e.message;
      push(B.kblock, 0);
      setResult('input.keyboard_lock', 'fail', `keyboard.lock failed: ${e.message}`);
    }
  });
  $('page-exit-fullscreen').addEventListener('click', () => document.fullscreenElement && document.exitFullscreen());
  $('copy-results').addEventListener('click', async () => {
    const text = JSON.stringify(Object.fromEntries(results), null, 1);
    try { await navigator.clipboard.writeText(text); log('results copied'); } catch { log(text); }
  });
}

// ---- Blocked-worker probe ------------------------------------------------------------------------

let rafCount = 0;
let rafTimes = [];
(function tick(t) { rafCount++; rafTimes.push(t); if (rafTimes.length > 600) rafTimes.shift(); requestAnimationFrame(tick); })(0);

const probe = new OffscreenCanvas(1, 1).getContext('2d', { willReadFrequently: true });
function sampleCanvas() {
  try {
    probe.clearRect(0, 0, 1, 1);
    probe.drawImage(canvas, canvas.width / 2, canvas.height / 2, 1, 1, 0, 0, 1, 1);
    const d = probe.getImageData(0, 0, 1, 1).data;
    return [d[0], d[1], d[2], d[3]];
  } catch (e) {
    return null;
  }
}
const COLORS = { magenta: [255, 0, 255], cyan: [0, 255, 255], yellow: [255, 255, 0], green: [0, 255, 0], blue: [0, 0, 255], white: [255, 255, 255] };
function classify(px) {
  if (!px) return 'unreadable';
  if (px[3] === 0) return 'transparent';
  for (const [name, c] of Object.entries(COLORS)) if (Math.abs(px[0] - c[0]) + Math.abs(px[1] - c[1]) + Math.abs(px[2] - c[2]) < 30) return name;
  return 'scene';
}

let block = null;
globalThis.labBlockArm = () => {
  block = { samples: [], timer: setInterval(() => block && block.samples.push({ t: epochNow(), kind: classify(sampleCanvas()) }), 8) };
};
globalThis.labBlockStart = (start, ms) => {
  if (!block) globalThis.labBlockArm();
  Object.assign(block, { start, ms, rafStart: rafCount, receivedAt: epochNow() });
  $('overlay').hidden = false;
  $('overlay-text').textContent = `for ${ms} ms; this overlay is drawn by the page`;
};
globalThis.labBlockEnd = (start, end) => {
  if (!block) return;
  Object.assign(block, { end, rafEnd: rafCount, endReceivedAt: epochNow() });
  $('overlay').hidden = true;
  setTimeout(evaluateBlock, 600);
};

function evaluateBlock() {
  const b = block;
  block = null;
  clearInterval(b.timer);
  const during = b.samples.filter((s) => s.t > b.start + 20 && s.t < b.end - 5);
  const before = b.samples.filter((s) => s.t < b.start);
  const after = b.samples.filter((s) => s.t > b.end);
  const count = (arr) => arr.reduce((m, s) => ((m[s.kind] = (m[s.kind] || 0) + 1), m), {});
  const readable = before.some((s) => s.kind === 'magenta');
  const fresh = during.filter((s) => ['cyan', 'yellow', 'green', 'blue', 'white'].includes(s.kind));
  const blockMs = b.end - b.start;
  const fps = ((b.rafEnd - b.rafStart) * 1000) / blockMs;
  const firstAfter = after.find((s) => s.kind !== 'magenta');
  const data = {
    block_ms: blockMs, samples_before: count(before), samples_during: count(during), samples_after: count(after.slice(0, 60)),
    first_new_frame_after_block_ms: firstAfter ? firstAfter.t - b.end : null, first_new_kind: firstAfter?.kind,
    page_raf_during_block: b.rafEnd - b.rafStart, page_fps_during_block: fps,
  };
  if (!readable) {
    return setResult('gpu.block', 'unverified', `The page could not read the canvas (samples before the block: ${JSON.stringify(count(before))}); page rAF ran at ${fmt(fps, 0)} fps during the ${fmt(blockMs, 0)} ms block`, data);
  }
  if (fresh.length === 0) {
    setResult('gpu.block', 'pass',
      `No frame is presented while the worker is blocked (the plan's assumption holds): for ${fmt(blockMs, 0)} ms the canvas kept the frame from before the block, although the worker submitted 5 new frames; the first new frame (${firstAfter?.kind ?? '?'}) appeared ${fmt(data.first_new_frame_after_block_ms, 0)} ms after the worker returned. The page's own rAF kept running at ${fmt(fps, 0)} fps, so a page-drawn loading overlay works.`,
      data);
  } else {
    setResult('gpu.block', 'info', `Frames reached the screen while the worker was blocked (${fresh.length} samples: ${JSON.stringify(count(fresh))}); the plan's assumption does not hold here`, data);
  }
}

// ---- Storage -------------------------------------------------------------------------------------

async function persist() {
  if (!navigator.storage?.persist) return setResult('storage.persist', 'fail', 'navigator.storage.persist is missing');
  const before = await navigator.storage.persisted();
  const granted = await navigator.storage.persist();
  setResult('storage.persist', granted ? 'pass' : 'info',
    granted ? `persistent storage granted (was ${before ? 'already' : 'not'} persisted before)` :
      'persist() returned false: the browser kept storage best-effort (Chrome grants it by site engagement, bookmark or install; no prompt)',
    { persisted_before: before, granted });
}

async function estimate() {
  if (!navigator.storage?.estimate) return setResult('storage.estimate', 'fail', 'navigator.storage.estimate is missing');
  const e = await navigator.storage.estimate();
  const gib = (v) => (v / 2 ** 30).toFixed(1);
  // What this origin keeps in OPFS (files with an open sync handle cannot be read and show null).
  const files = {};
  const walk = async (dir, prefix) => {
    for await (const [name, h] of dir.entries()) {
      if (h.kind === 'directory') await walk(h, `${prefix}${name}/`);
      else files[prefix + name] = await h.getFile().then((f) => f.size, () => null);
    }
  };
  try { await walk(await navigator.storage.getDirectory(), ''); } catch (err) { files.error = err.message; }
  setResult('storage.estimate', e.quota > 20 * 2 ** 30 ? 'pass' : 'info',
    `quota ${gib(e.quota)} GiB, usage ${(e.usage / 2 ** 20).toFixed(1)} MiB${e.quota > 20 * 2 ** 30 ? ' (room for the 16 GB data set)' : ' (smaller than the 16 GB data set)'}`,
    { quota: e.quota, usage: e.usage, usageDetails: e.usageDetails ?? null, opfs_files: files });
}

// ---- DualSense -----------------------------------------------------------------------------------

let pad = null;
let pendingPadCheck = null;
let lastFeedbackSeq = 0;
const PRESETS = [
  { name: 'off', left: { type: TriggerType.Off }, right: { type: TriggerType.Off } },
  { name: 'feedback', left: { type: TriggerType.Feedback, start: 0.3, strength: 0.18 }, right: { type: TriggerType.Feedback, start: 0.4, strength: 0.3 } },
  { name: 'weapon', left: { type: TriggerType.Feedback, start: 0.3, strength: 0.18 }, right: { type: TriggerType.Weapon, start: 0.4, end: 0.62, strength: 0.55 } },
  { name: 'vibration', left: { type: TriggerType.Feedback, start: 0.25, strength: 0.4 }, right: { type: TriggerType.Vibration, start: 0.4, amplitude: 0.55, frequency: 0.55 } },
  { name: 'slope', left: { type: TriggerType.Slope, start: 0.2, end: 0.8, strength: 0.2, end_strength: 1 }, right: { type: TriggerType.Slope, start: 0.2, end: 0.8, strength: 0.2, end_strength: 1 } },
];
const hex = (a) => Array.from(a, (b) => b.toString(16).padStart(2, '0')).join(' ');
const BUTTON_NAMES = Object.fromEntries(Object.entries(PadButton).map(([k, v]) => [v, k]));

async function sendPad(fb, label) {
  const out = buildOutputReport(fb, pad ? pad.bluetooth : false, pad ? pad.seq : 0);
  $('pad-output').textContent = `${label}\nreport 0x${out.reportId.toString(16).padStart(2, '0')} (${out.data.length} bytes${pad ? '' : ', not sent: no pad'})\n${hex(out.data)}`;
  if (!pad) return;
  try { await pad.send(fb); } catch (e) { log(`sendReport failed: ${e.message}`); }
}

function showPadState(s) {
  const names = [];
  for (let i = 0; i < 21; i++) if ((s.buttons >>> i) & 1) names.push(BUTTON_NAMES[i]);
  const f = s.finger.map((t, i) => `touch${i} ${t.down ? `${fmt(t.x, 3)}, ${fmt(t.y, 3)}` : 'up'}`).join('  ');
  $('pad-state').textContent =
    `${pad ? `${pad.name}, ${pad.bluetooth ? 'Bluetooth' : 'USB'}, ${s.full ? 'full' : 'reduced'} reports, calibration ${pad.calibration.valid ? 'applied' : 'not applied'}\nreports ${pad.reports}, bad CRC ${pad.badCrc}\n` : ''}` +
    `buttons ${names.join(' ') || '–'}\nleft ${fmt(s.left[0], 2)}, ${fmt(s.left[1], 2)}   right ${fmt(s.right[0], 2)}, ${fmt(s.right[1], 2)}\n` +
    `L2 ${fmt(s.l2, 2)}  R2 ${fmt(s.r2, 2)}\n${f}\ngyro rad/s ${s.gyro.map((v) => fmt(v, 3)).join(', ')}\naccel m/s² ${s.accel.map((v) => fmt(v, 2)).join(', ')}\n` +
    `battery ${s.battery < 0 ? 'unknown' : `${Math.round(s.battery * 100)}%`}${s.charging ? ', charging' : ''}`;
}

function installPad() {
  if (!DualSense.supported()) {
    $('pad-status').textContent = 'This browser has no WebHID (Chrome or Edge on desktop needed).';
    for (const b of document.querySelectorAll('#pad-connect')) b.disabled = true;
  }
  $('pad-connect').addEventListener('click', async () => {
    try {
      const p = await DualSense.request();
      if (!p) return;
      await connectPad(p);
    } catch (e) {
      $('pad-status').textContent = `Connect failed: ${e.message}`;
    }
  });
  $('pad-disconnect').addEventListener('click', async () => {
    if (!pad) return;
    await pad.close();
    pad = null;
    $('pad-status').textContent = 'Disconnected.';
    $('pad-disconnect').disabled = true;
  });
  $('pad-selftest').addEventListener('click', runPadSelfTest);
  $('pad-synthetic').addEventListener('click', runSyntheticPad);
  for (const b of document.querySelectorAll('[data-pad]')) {
    b.addEventListener('click', () => {
      const k = b.dataset.pad;
      if (k === 'rumble') {
        sendPad({ flags: FEEDBACK_FLAGS.rumble, rumble: { low: 0.8, high: 0.5 } }, 'rumble low 0.8 high 0.5');
        setTimeout(() => sendPad({ flags: FEEDBACK_FLAGS.rumble, rumble: { low: 0, high: 0 } }, 'rumble off'), 1000);
      } else if (k === 'leds') {
        sendPad({ flags: FEEDBACK_FLAGS.playerLeds, playerLeds: 0x15 }, 'player LEDs 1, 3, 5');
      } else {
        const led = { red: [255, 0, 0], green: [0, 255, 0], blue: [0, 0, 255] }[k];
        sendPad({ flags: FEEDBACK_FLAGS.led, led }, `light bar ${k}`);
      }
    });
  }
  for (const b of document.querySelectorAll('[data-trigger]')) {
    b.addEventListener('click', () => {
      const p = PRESETS[Number(b.dataset.trigger)];
      sendPad({ flags: FEEDBACK_FLAGS.triggers, triggers: { left: p.left, right: p.right } }, `triggers: ${p.name}`);
    });
  }
  // Follow the worker's PadFeedbackShared block.
  (function poll() {
    if (bridge && $('pad-follow').checked) {
      const fb = readFeedbackShared(heap(), bridge.feedback);
      if (fb && fb.seq !== lastFeedbackSeq && fb.seq !== 0) {
        lastFeedbackSeq = fb.seq;
        sendPad(fb, `worker feedback seq ${fb.seq}`);
      }
    }
    requestAnimationFrame(poll);
  })();
  DualSense.supported() && DualSense.granted().then((list) => list.length && connectPad(list[0])).catch(() => {});
  buildChecklist();
}

async function connectPad(p) {
  await p.open();
  pad = p;
  if (bridge) p.attachShared(heap(), bridge.pad);
  p.addEventListener('state', (e) => showPadState(e.detail));
  p.addEventListener('log', (e) => log(e.detail));
  $('pad-status').textContent = `${p.name} connected over ${p.bluetooth ? 'Bluetooth' : 'USB'}; calibration ${p.calibration.valid ? 'applied' : 'not applied'}.`;
  $('pad-disconnect').disabled = false;
  log(`DualSense: ${p.name} ${p.bluetooth ? 'Bluetooth' : 'USB'}, calibration ${p.calibration.valid ? 'valid' : 'invalid or missing'}`);
}

function runPadSelfTest() {
  const r = runSelfTest();
  const failed = r.filter((x) => !x.ok);
  setResult('pad.selftest', failed.length ? 'fail' : 'pass',
    failed.length ? `${failed.length} of ${r.length} checks failed: ${failed.map((x) => x.name).join('; ')}` :
      `${r.length} checks: USB and Bluetooth input parsing, calibration, CRC32, the five trigger encodings, output report layouts, shared-memory layout`,
    { checks: r });
}

// Writes synthetic reports into PadShared and checks that the worker's PadSnapshot matches.
let syntheticState = null;
function runSyntheticPad() {
  if (!bridge) return setResult('pad.shared', 'fail', 'no PadShared block: wasm not attached');
  if (pad) return log('real pad connected; synthetic feed skipped');
  let state = emptyState();
  const payload = syntheticUsbPayload();
  let n = 0;
  const timer = setInterval(() => {
    payload[0] = 128 + Math.round(100 * Math.sin(n / 10));
    payload[7] = (payload[7] & 0x0f) | (n % 2 ? 0x20 : 0x80); // alternate cross and triangle
    state = parseInputReport(0x01, payload, undefined, state);
    writePadShared(heap(), bridge.pad, state, { bluetooth: false, calibrated: false });
    if (++n >= 60) {
      clearInterval(timer);
      syntheticState = state;
      const reports = new Uint32Array(heap(), bridge.pad + 96, 1)[0];
      pendingPadCheck = { reports, t: performance.now() };
    }
  }, 16);
}

function checkSyntheticPad(snap) {
  const s = syntheticState;
  pendingPadCheck = null;
  const ok = snap.buttons === s.buttons && Math.abs(snap.left[0] - s.left[0]) < 1e-4 && Math.abs(snap.left[1] - s.left[1]) < 1e-4 &&
    Math.abs(snap.triggers[0] - s.l2) < 1e-4 && Math.abs(snap.accel[1] - s.accel[1]) < 1e-3 && snap.touch0[0] === 1;
  setResult('pad.shared', ok ? 'pass' : 'fail',
    ok ? `60 synthetic USB reports parsed on the page and written to PadShared; the worker read them into a gameinput::PadSnapshot that matches (buttons 0x${(snap.buttons >>> 0).toString(16)}, left ${fmt(snap.left[0], 3)}, ${fmt(snap.left[1], 3)})`
      : 'the worker’s PadSnapshot differs from what the page wrote',
    { page: { buttons: s.buttons, left: s.left, l2: s.l2, accel: s.accel }, worker: snap });
}

const CHECKS = [
  'Connect DualSense and pick the pad; the status names the transport.',
  'Every button lights its name, including PS, touchpad click, mute, Create and Options (Edge: Fn and back buttons).',
  'Sticks reach ±1.00 at full travel; pushing down gives positive y.',
  'L2 and R2 run smoothly from 0.00 to 1.00.',
  'Two fingers on the touchpad show two points, 0..1 from the top-left corner.',
  'Calibration shows “applied”; at rest the gyro reads near 0 rad/s and the accelerometer about 9.8 on the axis pointing up.',
  'Battery shows a percentage (and “charging” on a charging cable).',
  'Rumble 1 s runs both motors.',
  'Light bar turns red, green and blue; Player LEDs shows lights 1, 3 and 5.',
  'Triggers: Feedback resists from about a third of travel; Weapon clicks between 40% and 62%; Vibration buzzes from 40%; Slope ramps up; Off is free.',
  'With “Follow worker feedback” ticked, pressing T on the canvas cycles the worker’s presets on the pad.',
  'The worker’s PadSnapshot card follows the sticks and buttons.',
];

function buildChecklist() {
  const saved = JSON.parse(localStorage.getItem('lab.checklist') || '{}');
  const root = $('checklist');
  for (const transport of ['USB', 'Bluetooth']) {
    const box = document.createElement('div');
    box.innerHTML = `<h3>${transport}${transport === 'Bluetooth' ? ' (pair the pad in the OS Bluetooth settings first)' : ' (cable)'}</h3>`;
    const items = transport === 'Bluetooth' ? [...CHECKS, 'Bad CRC stays at 0; the blue pairing pulse stops once connected.'] : CHECKS;
    items.forEach((text, i) => {
      const key = `${transport}.${i}`;
      const label = document.createElement('label');
      label.innerHTML = '<input type="checkbox"><span></span>';
      label.querySelector('span').textContent = text;
      const input = label.querySelector('input');
      input.checked = !!saved[key];
      input.addEventListener('change', () => { saved[key] = input.checked; localStorage.setItem('lab.checklist', JSON.stringify(saved)); updateChecklistRow(); });
      box.appendChild(label);
    });
    root.appendChild(box);
  }
  updateChecklistRow();
}

function updateChecklistRow() {
  const boxes = [...document.querySelectorAll('#checklist input')];
  const done = boxes.filter((b) => b.checked).length;
  setResult('pad.device', done === boxes.length ? 'pass' : 'unverified',
    done === boxes.length ? 'all manual checks ticked by the owner (USB and Bluetooth)' : `needs hardware: ${done} of ${boxes.length} manual checks ticked (checklist below)`);
}

// ---- Start ---------------------------------------------------------------------------------------

buildTable();
installControls();
installBridge();
installPad();
runPadSelfTest();
globalThis.lab = {
  results: () => Object.fromEntries(results), command, setResult,
  health: () => ({ msSinceWorkerReport: performance.now() - lastLiveAt, rafCount, visibility: document.visibilityState, log: $('log').innerText.split('\n').slice(-8) }),
};
if (environment()) {
  loadWasm();
  persist().then(estimate).catch((e) => setResult('storage.persist', 'fail', e.message));
}
