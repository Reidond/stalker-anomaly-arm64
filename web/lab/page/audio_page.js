// Page side of the audio lab: starts the AudioContext on a click and measures where SDL3's
// Emscripten backend runs its mix callback (a ScriptProcessorNode handler on this main thread).
const out = document.getElementById('out');
const state = { open: null, progress: null, callbacks: 0, cbMs: [], node: null };
globalThis.audioState = state;

function render() {
  const ctx = globalThis.Module?.SDL3?.audioContext;
  const ms = state.cbMs.slice(-200).sort((a, b) => a - b);
  const median = ms.length ? ms[ms.length >> 1] : NaN;
  state.summary = {
    open: state.open,
    progress: state.progress,
    audioContext: ctx ? { state: ctx.state, sampleRate: ctx.sampleRate, baseLatency: ctx.baseLatency } : null,
    scriptProcessor: state.node ? { bufferSize: state.node.bufferSize, callbacksOnMainThread: state.callbacks, medianCallbackMs: median } : null,
  };
  out.textContent = JSON.stringify(state.summary, null, 1);
}

// Wrap SDL's onaudioprocess handler (set on the main thread) to count and time it.
function hookNode() {
  const node = globalThis.Module?.SDL3?.audio_playback?.scriptProcessorNode;
  if (!node || node.labHooked) return;
  const inner = node.onaudioprocess;
  node.onaudioprocess = (e) => {
    const t0 = performance.now();
    inner(e);
    state.cbMs.push(performance.now() - t0);
    state.callbacks++;
  };
  node.labHooked = true;
  state.node = node;
}

globalThis.audioReport = (r) => {
  if (r.type === 'open') state.open = r;
  else state.progress = r;
  hookNode();
  render();
};

globalThis.Module = { print: (t) => console.log(t), printErr: (t) => console.warn(t) };
const s = document.createElement('script');
s.src = 'audio_lab.js';
s.onerror = () => { out.textContent = 'audio_lab.js missing: run web/lab/build.sh (the audio part needs OpenAL Soft)'; };
document.body.appendChild(s);

document.getElementById('start').addEventListener('click', () => {
  const ctx = globalThis.Module?.SDL3?.audioContext;
  if (ctx) ctx.resume().then(render);
});
setInterval(() => { hookNode(); render(); }, 500);
