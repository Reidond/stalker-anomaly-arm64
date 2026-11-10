// DualSense over WebHID, as a standalone ES module (page main thread; Chrome and Edge).
//
// Input: USB report 0x01 and Bluetooth report 0x31 (plus the reduced Bluetooth 0x01 report sent
// before the pad is switched to full reports) parsed into the fields of gameinput::PadSnapshot
// (src/Platform/GameInput/include/GameInput/Types.h), with IMU calibration from feature report 0x05.
// Output: rumble, light bar, player LEDs and adaptive trigger effects for the modes the port models
// (gameinput::TriggerEffect in Feedback.h: Off, Feedback, Weapon, Vibration, Slope). Bluetooth
// output reports carry a CRC32.
//
// Shared memory: writePadShared() fills the PadShared block and readFeedbackShared() reads the
// PadFeedbackShared block defined in ../src/pad_shared.h; the offsets below mirror that header.
//
// Report layouts follow SDL's HIDAPI PS5 driver (external/SDL/src/joystick/hidapi/SDL_hidapi_ps5.c,
// zlib licence). Trigger effect bytes follow the publicly documented DualSense trigger modes
// (0x05 off, 0x21 feedback / multi-position feedback, 0x25 weapon, 0x26 vibration); see
// docs/web-port/BROWSER-LAB.md. No code here is copied from elsewhere.

export const SONY_VENDOR_ID = 0x054c;
export const PRODUCTS = { 0x0ce6: 'DualSense', 0x0df2: 'DualSense Edge' };
export const HID_FILTERS = Object.keys(PRODUCTS).map((pid) => ({ vendorId: SONY_VENDOR_ID, productId: Number(pid) }));

// gameinput::PadButton bit positions.
export const PadButton = {
  South: 0, East: 1, West: 2, North: 3, LeftShoulder: 4, RightShoulder: 5, LeftStick: 6, RightStick: 7,
  DpadUp: 8, DpadDown: 9, DpadLeft: 10, DpadRight: 11, Back: 12, Start: 13, Guide: 14, Touchpad: 15,
  Misc1: 16, PaddleLeft1: 17, PaddleLeft2: 18, PaddleRight1: 19, PaddleRight2: 20,
};

// gameinput::TriggerEffect::Type.
export const TriggerType = { Off: 0, Feedback: 1, Weapon: 2, Vibration: 3, Slope: 4 };

// pad_shared.h
export const PAD_LAYOUT = {
  size: 112, seq: 0, flags: 4, buttons: 8, left: 12, right: 20, l2: 28, r2: 32,
  finger: 36, fingerStride: 16, gyro: 68, accel: 80, battery: 92, reportCount: 96, epochMs: 104,
};
export const PAD_FLAGS = { connected: 1, hasTouchpad: 2, hasGyro: 4, hasAccel: 8, charging: 16, bluetooth: 32, calibrated: 64 };
export const FEEDBACK_LAYOUT = {
  size: 76, seq: 0, flags: 4, motorLow: 8, motorHigh: 12, led: 16, playerLeds: 19, left: 20, right: 48,
  trigger: { type: 0, start: 4, end: 8, strength: 12, endStrength: 16, amplitude: 20, frequency: 24 },
};
export const FEEDBACK_FLAGS = { rumble: 1, led: 2, triggers: 4, playerLeds: 8 };

const GYRO_RES_PER_DEGREE = 1024;
const ACCEL_RES_PER_G = 8192;
const STANDARD_GRAVITY = 9.80665;
const TOUCH_WIDTH = 1920;
const TOUCH_HEIGHT = 1070;

// ---- CRC32 (IEEE, reflected, as zlib) -------------------------------------------------------------

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();

export function crc32(bytes, crc = 0) {
  let c = ~crc >>> 0;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}

// Bluetooth reports include the HID transaction header in the CRC: 0xA1 for input, 0xA2 for output,
// 0xA3 for feature reports, followed by the report id and the payload without its last 4 bytes.
export function bluetoothCrc(header, reportId, payload) {
  const head = crc32(Uint8Array.of(header, reportId));
  return crc32(payload.subarray(0, payload.length - 4), head);
}

// ---- Calibration (feature report 0x05) -----------------------------------------------------------

export function defaultCalibration() {
  return { valid: false, imu: [] };
}

// `bytes`: the feature report with the report id as byte 0 (receiveFeatureReport's DataView may or
// may not include it; normalizeFeature() fixes that).
export function parseCalibration(bytes) {
  if (bytes.length < 35) return defaultCalibration();
  const s16 = (i) => ((bytes[i] | (bytes[i + 1] << 8)) << 16) >> 16;
  const pitchBias = s16(1), yawBias = s16(3), rollBias = s16(5);
  const pitchPlus = s16(7), pitchMinus = s16(9), yawPlus = s16(11), yawMinus = s16(13), rollPlus = s16(15), rollMinus = s16(17);
  const speedPlus = s16(19), speedMinus = s16(21);
  const accXPlus = s16(23), accXMinus = s16(25), accYPlus = s16(27), accYMinus = s16(29), accZPlus = s16(31), accZMinus = s16(33);
  const numerator = (speedPlus + speedMinus) * GYRO_RES_PER_DEGREE;
  const accel = (plus, minus) => {
    const range = plus - minus;
    return { bias: plus - Math.trunc(range / 2), sens: (2 * ACCEL_RES_PER_G) / range };
  };
  const imu = [
    { bias: pitchBias, sens: numerator / (pitchPlus - pitchMinus) },
    { bias: yawBias, sens: numerator / (yawPlus - yawMinus) },
    { bias: rollBias, sens: numerator / (rollPlus - rollMinus) },
    accel(accXPlus, accXMinus),
    accel(accYPlus, accYMinus),
    accel(accZPlus, accZMinus),
  ];
  // Some pads ship bad calibration; SDL ignores it in that case, and so do we.
  const valid = imu.every((c, i) => Number.isFinite(c.sens) && Math.abs(c.bias) <= 1024 &&
    Math.abs(1 - c.sens / (i < 3 ? 64 : 1)) <= 0.5);
  return { valid, imu };
}

function applyCalibration(cal, index, raw) {
  let v;
  if (cal.valid) v = (raw - cal.imu[index].bias) * cal.imu[index].sens;
  else v = index < 3 ? raw * 64 : raw;
  return index < 3 ? (v / GYRO_RES_PER_DEGREE) * (Math.PI / 180) : (v / ACCEL_RES_PER_G) * STANDARD_GRAVITY;
}

// ---- Input reports ------------------------------------------------------------------------------

export function emptyState() {
  return {
    connected: false, buttons: 0, left: [0, 0], right: [0, 0], l2: 0, r2: 0,
    hasTouchpad: false, finger: [{ down: false, x: 0, y: 0, pressure: 0 }, { down: false, x: 0, y: 0, pressure: 0 }],
    hasGyro: false, gyro: [0, 0, 0], hasAccel: false, accel: [0, 0, 0], battery: -1, charging: false,
    full: false, crcOk: true, counter: 0, sensorTimestamp: 0,
  };
}

const axis = (v) => Math.max(-1, Math.min(1, (v * 257 - 32768) / 32767));
const HAT_BITS = [
  [PadButton.DpadUp], [PadButton.DpadUp, PadButton.DpadRight], [PadButton.DpadRight], [PadButton.DpadRight, PadButton.DpadDown],
  [PadButton.DpadDown], [PadButton.DpadDown, PadButton.DpadLeft], [PadButton.DpadLeft], [PadButton.DpadLeft, PadButton.DpadUp],
];

function parseButtons(b0, b1, b2) {
  let m = 0;
  const set = (bit, on) => { if (on) m |= 1 << bit; };
  for (const bit of HAT_BITS[b0 & 0x0f] || []) set(bit, true);
  const face = b0 >> 4;
  set(PadButton.West, face & 0x01);
  set(PadButton.South, face & 0x02);
  set(PadButton.East, face & 0x04);
  set(PadButton.North, face & 0x08);
  set(PadButton.LeftShoulder, b1 & 0x01);
  set(PadButton.RightShoulder, b1 & 0x02);
  set(PadButton.Back, b1 & 0x10);
  set(PadButton.Start, b1 & 0x20);
  set(PadButton.LeftStick, b1 & 0x40);
  set(PadButton.RightStick, b1 & 0x80);
  set(PadButton.Guide, b2 & 0x01);
  set(PadButton.Touchpad, b2 & 0x02);
  set(PadButton.Misc1, b2 & 0x04);
  // DualSense Edge: Fn buttons and back paddles, mapped as SDL maps them.
  set(PadButton.PaddleLeft2, b2 & 0x10);
  set(PadButton.PaddleRight2, b2 & 0x20);
  set(PadButton.PaddleLeft1, b2 & 0x40);
  set(PadButton.PaddleRight1, b2 & 0x80);
  return m >>> 0;
}

function parseFull(state, d, o, cal) {
  const s16 = (i) => ((d[o + i] | (d[o + i + 1] << 8)) << 16) >> 16;
  state.full = true;
  state.left = [axis(d[o]), axis(d[o + 1])];
  state.right = [axis(d[o + 2]), axis(d[o + 3])];
  state.l2 = d[o + 4] / 255;
  state.r2 = d[o + 5] / 255;
  state.counter = d[o + 6];
  state.buttons = parseButtons(d[o + 7], d[o + 8], d[o + 9]);
  state.gyro = [0, 1, 2].map((k) => applyCalibration(cal, k, s16(15 + 2 * k)));
  state.accel = [0, 1, 2].map((k) => applyCalibration(cal, 3 + k, s16(21 + 2 * k)));
  state.hasGyro = state.hasAccel = true;
  state.sensorTimestamp = (d[o + 27] | (d[o + 28] << 8) | (d[o + 29] << 16) | (d[o + 30] << 24)) >>> 0;
  state.hasTouchpad = true;
  for (let f = 0; f < 2; f++) {
    const c = o + 32 + f * 4;
    const down = (d[c] & 0x80) === 0;
    const x = d[c + 1] | ((d[c + 2] & 0x0f) << 8);
    const y = (d[c + 2] >> 4) | (d[c + 3] << 4);
    state.finger[f] = { down, x: x / TOUCH_WIDTH, y: y / TOUCH_HEIGHT, pressure: down ? 1 : 0 };
  }
  const status = (d[o + 52] >> 4) & 0x0f, level = d[o + 52] & 0x0f;
  state.charging = status === 1;
  state.battery = status === 0 || status === 1 ? Math.min(level * 10 + 5, 100) / 100 : status === 2 ? 1 : -1;
}

// Parses one input report. `data` is the payload without the report id (as WebHID delivers it).
// Returns the updated state, or null for reports this module does not handle.
export function parseInputReport(reportId, data, cal = defaultCalibration(), prev = emptyState()) {
  const state = { ...prev, finger: prev.finger.map((f) => ({ ...f })), connected: true };
  if (reportId === 0x01 && data.length >= 63) {
    parseFull(state, data, 0, cal); // USB
  } else if (reportId === 0x31 && data.length >= 77) {
    state.crcOk = bluetoothCrc(0xa1, 0x31, data) ===
      ((data[73] | (data[74] << 8) | (data[75] << 16) | (data[76] << 24)) >>> 0);
    if (!state.crcOk) return null;
    parseFull(state, data, 1, cal); // Bluetooth full report: byte 0 is the sequence/tag byte
  } else if (reportId === 0x01 && data.length >= 9) {
    // Bluetooth before full reports are enabled: sticks, buttons and triggers only.
    state.full = false;
    state.left = [axis(data[0]), axis(data[1])];
    state.right = [axis(data[2]), axis(data[3])];
    state.buttons = parseButtons(data[4], data[5], data[6] & 0x03);
    state.l2 = data[7] / 255;
    state.r2 = data[8] / 255;
    state.hasGyro = state.hasAccel = state.hasTouchpad = false;
  } else {
    return null;
  }
  return state;
}

// ---- Output reports -----------------------------------------------------------------------------

const clamp01 = (v) => Math.min(1, Math.max(0, Number(v) || 0));
const zoneOf = (pos) => Math.round(clamp01(pos) * 9); // 10 zones along the trigger travel
const level8 = (v) => Math.round(clamp01(v) * 8);      // 0 = none, 1..8

// Vibration frequency: gameinput keeps it as 0..1 like GameController; the pad wants Hz.
export const VIBRATION_MAX_HZ = 40;

function zonesEffect(mode, strengths) {
  // strengths[z] in 0..8 for z in 0..9: active zone bitmask plus 3 bits per zone (value - 1).
  let active = 0, packed = 0n;
  strengths.forEach((s, z) => {
    if (s > 0) {
      active |= 1 << z;
      packed |= BigInt(s - 1) << BigInt(3 * z);
    }
  });
  const out = new Uint8Array(11);
  out[0] = mode;
  out[1] = active & 0xff;
  out[2] = active >> 8;
  for (let i = 0; i < 4; i++) out[3 + i] = Number((packed >> BigInt(8 * i)) & 0xffn);
  return { bytes: out, packed };
}

// Encodes one gameinput::TriggerEffect ({type, start, end, strength, end_strength, amplitude,
// frequency}, all 0..1) into the pad's 11 effect bytes. Clamping matches DualSenseTriggers.mm.
export function encodeTriggerEffect(e) {
  const type = e ? e.type : TriggerType.Off;
  if (type === TriggerType.Feedback) {
    const strength = level8(e.strength);
    if (strength === 0) return encodeTriggerEffect(null);
    const z0 = zoneOf(e.start);
    return zonesEffect(0x21, Array.from({ length: 10 }, (_, z) => (z >= z0 ? strength : 0))).bytes;
  }
  if (type === TriggerType.Weapon) {
    const start = Math.min(clamp01(e.start), 0.94);
    const end = Math.min(1, Math.max(clamp01(e.end), start + 0.05));
    const strength = level8(e.strength);
    if (strength === 0) return encodeTriggerEffect(null);
    const z0 = Math.min(7, Math.max(2, zoneOf(start)));
    const z1 = Math.min(8, Math.max(z0 + 1, zoneOf(end)));
    const zones = (1 << z0) | (1 << z1);
    return Uint8Array.of(0x25, zones & 0xff, zones >> 8, strength - 1, 0, 0, 0, 0, 0, 0, 0);
  }
  if (type === TriggerType.Vibration) {
    const amplitude = level8(e.amplitude);
    const hz = Math.round(clamp01(e.frequency) * VIBRATION_MAX_HZ);
    if (amplitude === 0 || hz === 0) return encodeTriggerEffect(null);
    const z0 = zoneOf(e.start);
    const out = zonesEffect(0x26, Array.from({ length: 10 }, (_, z) => (z >= z0 ? amplitude : 0))).bytes;
    out[9] = hz;
    return out;
  }
  if (type === TriggerType.Slope) {
    // Multi-position feedback: strength ramps from `strength` at `start` to `end_strength` at `end`
    // and stays there to the end of travel.
    const start = Math.min(clamp01(e.start), 0.94);
    const end = Math.min(1, Math.max(clamp01(e.end), start + 0.05));
    const z0 = zoneOf(start), z1 = Math.max(z0 + 1, zoneOf(end));
    const s0 = Math.max(1, level8(e.strength)), s1 = Math.max(1, level8(e.end_strength));
    const strengths = Array.from({ length: 10 }, (_, z) => {
      if (z < z0) return 0;
      if (z >= z1) return s1;
      return Math.round(s0 + ((s1 - s0) * (z - z0)) / (z1 - z0));
    });
    return zonesEffect(0x21, strengths).bytes;
  }
  return Uint8Array.of(0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

// The 47-byte effects block shared by USB report 0x02 and Bluetooth report 0x31.
// fb: {rumble: {low, high}, led: [r, g, b], playerLeds, triggers: {left, right}, flags}
export function buildEffects(fb) {
  const e = new Uint8Array(47);
  if (fb.flags & FEEDBACK_FLAGS.rumble) {
    e[0] |= 0x01 | 0x02; // compatible vibration, haptics select (rumble instead of audio haptics)
    e[3] = Math.round(clamp01(fb.rumble.low) * 255);  // left motor: heavy, low frequency
    e[2] = Math.round(clamp01(fb.rumble.high) * 255); // right motor: light, high frequency
  }
  if (fb.flags & FEEDBACK_FLAGS.triggers) {
    e[0] |= 0x04 | 0x08; // right and left trigger effects valid
    e.set(encodeTriggerEffect(fb.triggers.right), 10);
    e.set(encodeTriggerEffect(fb.triggers.left), 21);
  }
  if (fb.flags & FEEDBACK_FLAGS.led) {
    e[1] |= 0x04; // light bar colour valid
    e[44] = fb.led[0];
    e[45] = fb.led[1];
    e[46] = fb.led[2];
  }
  if (fb.flags & FEEDBACK_FLAGS.playerLeds) {
    e[1] |= 0x10; // player indicator valid
    e[43] = (fb.playerLeds & 0x1f) | 0x20; // 0x20: switch instantly instead of fading
  }
  if (fb.releaseLeds) {
    e[38] |= 0x02; // valid_flag2: light bar setup
    e[41] = 0x02;  // light out: end the blue pairing animation so our colour shows (Bluetooth)
  }
  return e;
}

// Returns {reportId, data} ready for HIDDevice.sendReport (data excludes the report id).
export function buildOutputReport(fb, bluetooth, seq = 0) {
  const effects = buildEffects(fb);
  if (!bluetooth) {
    const data = new Uint8Array(47);
    data.set(effects);
    return { reportId: 0x02, data };
  }
  const data = new Uint8Array(77);
  data[0] = (seq & 0x0f) << 4; // sequence number in the high nibble
  data[1] = 0x10;              // fixed tag
  data.set(effects, 2);
  const c = bluetoothCrc(0xa2, 0x31, data);
  data[73] = c & 0xff;
  data[74] = (c >>> 8) & 0xff;
  data[75] = (c >>> 16) & 0xff;
  data[76] = (c >>> 24) & 0xff;
  return { reportId: 0x31, data };
}

// ---- Shared memory ------------------------------------------------------------------------------

// Writes `state` into PadShared at byte offset `base` of `buffer` (a SharedArrayBuffer, e.g. the
// wasm memory). Sequence lock: odd while writing.
export function writePadShared(buffer, base, state, extra = {}) {
  const L = PAD_LAYOUT;
  const u32 = new Uint32Array(buffer, base, L.size / 4);
  const f32 = new Float32Array(buffer, base, L.size / 4);
  const f64 = new Float64Array(buffer, base + L.epochMs, 1);
  const seq = Atomics.load(u32, 0);
  Atomics.store(u32, 0, (seq + 1) >>> 0);
  let flags = 0;
  if (state.connected) flags |= PAD_FLAGS.connected;
  if (state.hasTouchpad) flags |= PAD_FLAGS.hasTouchpad;
  if (state.hasGyro) flags |= PAD_FLAGS.hasGyro;
  if (state.hasAccel) flags |= PAD_FLAGS.hasAccel;
  if (state.charging) flags |= PAD_FLAGS.charging;
  if (extra.bluetooth) flags |= PAD_FLAGS.bluetooth;
  if (extra.calibrated) flags |= PAD_FLAGS.calibrated;
  u32[L.flags / 4] = flags;
  u32[L.buttons / 4] = state.buttons >>> 0;
  f32[L.left / 4] = state.left[0];
  f32[L.left / 4 + 1] = state.left[1];
  f32[L.right / 4] = state.right[0];
  f32[L.right / 4 + 1] = state.right[1];
  f32[L.l2 / 4] = state.l2;
  f32[L.r2 / 4] = state.r2;
  for (let i = 0; i < 2; i++) {
    const o = (L.finger + i * L.fingerStride) / 4;
    u32[o] = state.finger[i].down ? 1 : 0;
    f32[o + 1] = state.finger[i].x;
    f32[o + 2] = state.finger[i].y;
    f32[o + 3] = state.finger[i].pressure;
  }
  for (let i = 0; i < 3; i++) {
    f32[L.gyro / 4 + i] = state.gyro[i];
    f32[L.accel / 4 + i] = state.accel[i];
  }
  f32[L.battery / 4] = state.battery;
  u32[L.reportCount / 4] = (u32[L.reportCount / 4] + 1) >>> 0;
  f64[0] = extra.epochMs ?? performance.timeOrigin + performance.now();
  Atomics.store(u32, 0, (seq + 2) >>> 0);
}

// Reads PadFeedbackShared; returns null while the writer is mid-update.
export function readFeedbackShared(buffer, base) {
  const L = FEEDBACK_LAYOUT;
  const u32 = new Uint32Array(buffer, base, L.size / 4);
  const f32 = new Float32Array(buffer, base, L.size / 4);
  const u8 = new Uint8Array(buffer, base, L.size);
  const s0 = Atomics.load(u32, 0);
  if (s0 & 1) return null;
  const trig = (off) => ({
    type: u32[off / 4],
    start: f32[off / 4 + 1], end: f32[off / 4 + 2], strength: f32[off / 4 + 3], end_strength: f32[off / 4 + 4],
    amplitude: f32[off / 4 + 5], frequency: f32[off / 4 + 6],
  });
  const fb = {
    seq: s0,
    flags: u32[1],
    rumble: { low: f32[2], high: f32[3] },
    led: [u8[16], u8[17], u8[18]],
    playerLeds: u8[19],
    triggers: { left: trig(L.left), right: trig(L.right) },
  };
  return Atomics.load(u32, 0) === s0 ? fb : null;
}

// ---- Device -------------------------------------------------------------------------------------

function normalizeFeature(dv, reportId) {
  const b = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
  if (b[0] === reportId) return b;
  const out = new Uint8Array(b.length + 1);
  out[0] = reportId;
  out.set(b, 1);
  return out;
}

export class DualSense extends EventTarget {
  static supported() {
    return typeof navigator !== 'undefined' && 'hid' in navigator;
  }

  // Must be called from a user gesture (click).
  static async request() {
    const devices = await navigator.hid.requestDevice({ filters: HID_FILTERS });
    return devices.length ? new DualSense(devices[0]) : null;
  }

  static async granted() {
    const devices = await navigator.hid.getDevices();
    return devices.filter((d) => d.vendorId === SONY_VENDOR_ID && PRODUCTS[d.productId]).map((d) => new DualSense(d));
  }

  constructor(device) {
    super();
    this.device = device;
    this.name = PRODUCTS[device.productId] || device.productName;
    this.bluetooth = false;
    this.calibration = defaultCalibration();
    this.state = emptyState();
    this.reports = 0;
    this.badCrc = 0;
    this.seq = 0;
    this.lastOutput = null;
    this.shared = null; // {buffer, base}
    this.onReport = this.onReport.bind(this);
  }

  // Bluetooth DualSense collections declare input report 0x31; USB ones do not.
  detectTransport() {
    for (const c of this.device.collections || [])
      for (const r of c.inputReports || [])
        if (r.reportId === 0x31) return true;
    return false;
  }

  async open() {
    if (!this.device.opened) await this.device.open();
    this.bluetooth = this.detectTransport();
    // Reading the calibration report also switches a Bluetooth pad to full 0x31 reports.
    try {
      this.calibration = parseCalibration(normalizeFeature(await this.device.receiveFeatureReport(0x05), 0x05));
    } catch (e) {
      this.dispatchEvent(new CustomEvent('log', { detail: `calibration read failed: ${e.message}` }));
    }
    this.device.addEventListener('inputreport', this.onReport);
    // Over Bluetooth the light bar keeps its pairing animation until told otherwise.
    if (this.bluetooth) await this.send({ flags: 0, releaseLeds: true });
    return this;
  }

  async close() {
    this.device.removeEventListener('inputreport', this.onReport);
    if (this.device.opened) await this.device.close();
  }

  attachShared(buffer, base) {
    this.shared = { buffer, base };
  }

  onReport(event) {
    const data = new Uint8Array(event.data.buffer, event.data.byteOffset, event.data.byteLength);
    const next = parseInputReport(event.reportId, data, this.calibration, this.state);
    if (!next) {
      if (event.reportId === 0x31) this.badCrc++;
      return;
    }
    this.state = next;
    this.reports++;
    const epochMs = performance.timeOrigin + event.timeStamp;
    if (this.shared)
      writePadShared(this.shared.buffer, this.shared.base, next, { bluetooth: this.bluetooth, calibrated: this.calibration.valid, epochMs });
    this.dispatchEvent(new CustomEvent('state', { detail: next }));
  }

  // fb as for buildEffects().
  async send(fb) {
    const { reportId, data } = buildOutputReport(fb, this.bluetooth, this.seq);
    this.seq = (this.seq + 1) & 0x0f;
    this.lastOutput = { reportId, data };
    await this.device.sendReport(reportId, data);
    return this.lastOutput;
  }
}
