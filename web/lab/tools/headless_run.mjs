#!/usr/bin/env node
// Runs the browser lab in headless Chrome over the DevTools protocol, drives the input tests with
// trusted (CDP) input events, and prints every result row. Nothing appears on screen: no window,
// no focus change, no real pointer lock or fullscreen on the desktop.
//
//   web/lab/serve.py &                      # the lab on http://localhost:8642/
//   node web/lab/tools/headless_run.mjs     # [--url URL] [--chrome PATH] [--out FILE] [--fresh]
//
// The Chrome profile lives in build/web-lab/chrome-profile (gitignored); results go to
// build/web-lab/headless-results.json.
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const argv = process.argv.slice(2);
const opt = (name, def) => { const i = argv.indexOf(`--${name}`); return i >= 0 ? argv[i + 1] : def; };
const URL_ = opt('url', 'http://localhost:8642/');
const CHROME = opt('chrome', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome');
const OUT = opt('out', path.join(ROOT, 'build/web-lab/headless-results.json'));
const PROFILE = path.join(ROOT, 'build/web-lab/chrome-profile');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// --fresh: start from an empty profile (only a folder this script made, recognised by its marker).
if (argv.includes('--fresh') && fs.existsSync(path.join(PROFILE, '.web-lab-profile'))) fs.rmSync(PROFILE, { recursive: true });
fs.mkdirSync(PROFILE, { recursive: true });
fs.writeFileSync(path.join(PROFILE, '.web-lab-profile'), 'headless Chrome profile for web/lab/tools/headless_run.mjs\n');

const chrome = spawn(CHROME, [
  '--headless=new', `--user-data-dir=${PROFILE}`, '--remote-debugging-port=0', '--no-first-run',
  '--no-default-browser-check', '--mute-audio', '--window-size=1280,1000', 'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });
let shuttingDown = false;
const stop = () => { if (!shuttingDown) { shuttingDown = true; chrome.kill('SIGTERM'); } };
process.on('exit', stop);
process.on('SIGINT', () => { stop(); process.exit(130); });

const wsUrl = await new Promise((resolve, reject) => {
  let buf = '';
  const t = setTimeout(() => reject(new Error('Chrome did not print a DevTools URL')), 20000);
  chrome.stderr.on('data', (d) => {
    buf += d;
    const m = buf.match(/DevTools listening on (ws:\/\/\S+)/);
    if (m) { clearTimeout(t); resolve(m[1]); }
  });
  chrome.on('exit', (code) => reject(new Error(`Chrome exited (${code})`)));
});

// ---- Minimal CDP client --------------------------------------------------------------------------
const ws = new WebSocket(wsUrl);
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
let nextId = 1;
const pending = new Map();
const consoleLines = [];
ws.onmessage = (ev) => {
  const msg = JSON.parse(ev.data);
  if (msg.id && pending.has(msg.id)) {
    const { resolve, reject } = pending.get(msg.id);
    pending.delete(msg.id);
    msg.error ? reject(new Error(`${msg.error.message} ${msg.error.data ?? ''}`)) : resolve(msg.result);
  } else if (msg.method === 'Runtime.consoleAPICalled') {
    consoleLines.push(msg.params.args.map((a) => a.value ?? a.description ?? '').join(' '));
  }
};
const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
  const id = nextId++;
  pending.set(id, { resolve, reject });
  ws.send(JSON.stringify({ id, method, params, sessionId }));
});

const version = await send('Browser.getVersion');
const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
const s = (method, params) => send(method, params, sessionId);
await s('Runtime.enable');
await s('Page.enable');
// A background tab is "hidden": requestAnimationFrame stops on the page and on the worker, and the
// lab (like the engine) stops iterating. Make this tab the visible, focused one.
await send('Target.activateTarget', { targetId });
await s('Page.bringToFront');
await s('Emulation.setFocusEmulationEnabled', { enabled: true });
await s('Page.navigate', { url: URL_ });

const evaluate = async (expr) => {
  const r = await s('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
  return r.result.value;
};
const results = () => evaluate('globalThis.lab ? lab.results() : {}');
const waitFor = async (pred, ms, what) => {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    const r = await results();
    if (pred(r)) return r;
    await sleep(250);
  }
  console.warn(`timed out waiting for ${what}`);
  const r = await results();
  for (const row of Object.values(r)) if (row.status !== 'pending') console.warn(`  ${row.id} ${row.status}: ${row.summary}`);
  console.warn(consoleLines.slice(-25).map((l) => `  console: ${l}`).join('\n'));
  console.warn('  health:', JSON.stringify(await evaluate('lab.health()')));
  return r;
};
const settled = (r, id) => r[id] && !['pending', 'running'].includes(r[id].status);

// ---- Input helpers -------------------------------------------------------------------------------
const KEYS = {
  Tab: { key: 'Tab', code: 'Tab', keyCode: 9 }, Space: { key: ' ', code: 'Space', keyCode: 32, text: ' ' },
  Escape: { key: 'Escape', code: 'Escape', keyCode: 27 }, ArrowDown: { key: 'ArrowDown', code: 'ArrowDown', keyCode: 40 },
};
const keyFor = (k) => KEYS[k] ?? { key: k.toLowerCase(), code: `Key${k.toUpperCase()}`, keyCode: k.toUpperCase().charCodeAt(0), text: k.toLowerCase() };
async function press(k, holdMs = 30) {
  const d = keyFor(k);
  await s('Input.dispatchKeyEvent', { type: d.text ? 'keyDown' : 'rawKeyDown', key: d.key, code: d.code, windowsVirtualKeyCode: d.keyCode, text: d.text });
  await sleep(holdMs);
  await s('Input.dispatchKeyEvent', { type: 'keyUp', key: d.key, code: d.code, windowsVirtualKeyCode: d.keyCode });
  await sleep(40);
}
let mouse = { x: 0, y: 0 };
async function move(x, y, steps = 10) {
  const x0 = mouse.x, y0 = mouse.y;
  for (let i = 1; i <= steps; i++) {
    await s('Input.dispatchMouseEvent', { type: 'mouseMoved', x: x0 + ((x - x0) * i) / steps, y: y0 + ((y - y0) * i) / steps });
    await sleep(8);
  }
  mouse = { x, y };
}
async function click(x, y) {
  await move(x, y, 3);
  await s('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', buttons: 1, clickCount: 1 });
  await sleep(30);
  await s('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', buttons: 0, clickCount: 1 });
  await sleep(60);
}
const rect = (sel) => evaluate(`(() => { const r = document.querySelector(${JSON.stringify(sel)}).getBoundingClientRect(); return {x: r.x + r.width / 2, y: r.y + r.height / 2}; })()`);
const state = () => evaluate("({lock: document.pointerLockElement?.id ?? null, fs: document.fullscreenElement?.id ?? null})");

// ---- Run -----------------------------------------------------------------------------------------
console.log(`${version.product} (${version.userAgent})`);
let r = await waitFor((x) => settled(x, 'gpu.block') && settled(x, 'storage.opfs_bench'), 90000, 'GPU and storage tests');
await evaluate('window.scrollTo(0, 0)');

const c = await rect('#canvas');
await click(c.x, c.y);
for (const k of ['H', 'I', 'J', 'Tab', 'Space']) await press(k);
await move(c.x + 200, c.y + 100, 20);
await s('Input.dispatchMouseEvent', { type: 'mouseWheel', x: mouse.x, y: mouse.y, deltaX: 0, deltaY: 120 });
await sleep(100);
// SDL's own preventDefault, with the bridge's suppression off.
await evaluate("document.getElementById('bridge-capture').checked = false; document.getElementById('canvas').focus(); true");
await press('ArrowDown');
await press('Tab');
await evaluate("document.getElementById('bridge-capture').checked = true; window.scrollTo(0, 0); document.getElementById('canvas').focus(); true");
await sleep(400);

const notes = [];
const note = async (what) => { const st = await state(); notes.push({ what, ...st }); console.log(`${what}:`, st); return st; };

// SDL relative mode from the worker (L key), then relative motion.
await press('L');
await sleep(400);
if ((await note('after L (SDL_SetWindowRelativeMouseMode on the worker)')).lock !== 'canvas') {
  await click(c.x, c.y); // SDL re-grabs on click while relative mode is on
  await sleep(300);
  await note('after a click in SDL relative mode');
}
await move(c.x + 150, c.y - 80, 20);
await sleep(400);
// Control: does a CDP Escape behave like the user's Esc (which makes the browser drop the lock)?
await press('Escape');
await sleep(300);
await note('after CDP Escape while SDL holds the lock');
// The browser dropping the lock, as the app sees it: exitPointerLock() from outside SDL.
await evaluate('document.exitPointerLock(); true');
await sleep(1500);
await note('after the lock is dropped outside SDL');
await click(c.x, c.y); // SDL's documented recovery: re-grab on the next click
await sleep(300);
await note('after a click (SDL still in relative mode)');
await press('U');
await sleep(300);
await note('after U (relative mode off)');

// Page bridge pointer lock.
const lockBtn = await rect('#page-lock');
await click(lockBtn.x, lockBtn.y);
await sleep(300);
await note('after the page pointer lock button');
await move(lockBtn.x + 100, lockBtn.y - 100, 15);
await evaluate('document.exitPointerLock(); true');
await sleep(400);

// Page fullscreen + Keyboard Lock, Esc while locked; then the same Esc without the lock (control).
const fsBtn = await rect('#page-fullscreen');
await click(fsBtn.x, fsBtn.y);
await sleep(700);
await note('after the page fullscreen + keyboard lock button');
await press('Escape');
await sleep(400);
await note('after CDP Escape with keyboard lock');
await evaluate('navigator.keyboard.unlock(); true');
await press('Escape');
await sleep(600);
await note('after CDP Escape without keyboard lock (control)');
await evaluate('document.fullscreenElement && document.exitFullscreen().catch(() => {})');
await sleep(600);

// SDL fullscreen from the worker (F key).
await evaluate("document.getElementById('canvas').focus(); true");
await press('F');
await sleep(800);
await note('after F (SDL_SetWindowFullscreen on the worker)');
await evaluate('document.fullscreenElement && document.exitFullscreen().catch(() => {})');
await sleep(500);

r = await results();
const pageLog = await evaluate("document.getElementById('log').innerText");

// Item 5: the audio page (output muted by --mute-audio; the audio graph still runs).
let audio = null;
try {
  await s('Page.navigate', { url: new URL('audio.html', URL_).href });
  await sleep(2500);
  const b = await rect('#start');
  await click(b.x, b.y);
  await sleep(5000);
  audio = await evaluate('globalThis.audioState?.summary ?? null');
  console.log('audio lab:', JSON.stringify(audio));
} catch (e) {
  console.log(`audio lab failed: ${e.message}`);
}

fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, JSON.stringify({ browser: version, url: URL_, at: new Date().toISOString(), input_steps: notes, results: r, audio, page_log: pageLog.split('\n'), console: consoleLines.slice(-200) }, null, 1));
for (const row of Object.values(r)) console.log(`${row.id.padEnd(22)} ${row.status.padEnd(10)} ${row.summary}`);
console.log(`\npage log (last lines):\n${pageLog.split('\n').slice(-15).join('\n')}`);
console.log(`\nwrote ${OUT}`);
ws.close();
stop();
process.exit(0);
