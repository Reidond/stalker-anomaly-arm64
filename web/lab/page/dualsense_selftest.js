// Hardware-free checks of dualsense.js: synthetic USB and Bluetooth input reports, calibration,
// CRC32, trigger effect encodings and the shared-memory layout. Runs on the page (DualSense panel)
// and under Node (web/lab/test/dualsense.test.mjs).
import {
  PadButton, TriggerType, FEEDBACK_FLAGS, PAD_LAYOUT,
  crc32, bluetoothCrc, parseCalibration, parseInputReport, encodeTriggerEffect, buildOutputReport, writePadShared,
  readFeedbackShared, FEEDBACK_LAYOUT,
} from './dualsense.js';

const hex = (a) => Array.from(a, (b) => b.toString(16).padStart(2, '0')).join(' ');

// A USB 0x01 payload (63 bytes, no report id) with known contents.
export function syntheticUsbPayload() {
  const d = new Uint8Array(63);
  d[0] = 0; d[1] = 255; d[2] = 128; d[3] = 128;  // LX full left, LY full down, right stick centred
  d[4] = 255; d[5] = 0;                          // L2 full, R2 released
  d[7] = 0x22;                                   // hat 2 (right), cross
  d[8] = 0x01 | 0x20;                            // L1, Options
  d[9] = 0x01;                                   // PS
  const put16 = (i, v) => { d[i] = v & 0xff; d[i + 1] = (v >> 8) & 0xff; };
  put16(15, 1024); put16(17, -1024); put16(19, 0); // gyro
  put16(21, 0); put16(23, 8192); put16(25, 0);     // accel: +1 g on y
  d[32] = 0x01; d[33] = 960 & 0xff; d[34] = ((960 >> 8) & 0x0f) | ((535 & 0x0f) << 4); d[35] = 535 >> 4; // finger 0 down mid-pad
  d[36] = 0x80;                                  // finger 1 up
  d[52] = 0x15;                                  // charging, level 5
  return d;
}

export function syntheticBtPayload(usb = syntheticUsbPayload()) {
  const d = new Uint8Array(77);
  d[0] = 0x10; // sequence/tag byte
  d.set(usb.subarray(0, 63), 1);
  const c = bluetoothCrc(0xa1, 0x31, d);
  d[73] = c & 0xff; d[74] = (c >>> 8) & 0xff; d[75] = (c >>> 16) & 0xff; d[76] = (c >>> 24) & 0xff;
  return d;
}

export function syntheticCalibration() {
  const b = new Uint8Array(41);
  b[0] = 0x05;
  const put16 = (i, v) => { b[i] = v & 0xff; b[i + 1] = (v >> 8) & 0xff; };
  // biases 0; pitch/yaw/roll plus 8000 minus -8000; speed 500/500; accel +-8192.
  [7, 11, 15].forEach((i) => { put16(i, 8000); put16(i + 2, -8000); });
  put16(19, 500); put16(21, 500);
  [23, 27, 31].forEach((i) => { put16(i, 8192); put16(i + 2, -8192); });
  return b;
}

// Expected trigger bytes, worked out by hand from the documented encodings (see BROWSER-LAB.md).
export const TRIGGER_VECTORS = [
  { name: 'off', effect: { type: TriggerType.Off }, bytes: [0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0] },
  { name: 'feedback 0.30/0.18 (aim)', effect: { type: TriggerType.Feedback, start: 0.3, strength: 0.18 },
    bytes: [0x21, 0xf8, 0x03, 0, 0, 0, 0, 0, 0, 0, 0] },
  { name: 'weapon 0.40-0.62/0.55 (assault rifle)', effect: { type: TriggerType.Weapon, start: 0.4, end: 0.62, strength: 0.55 },
    bytes: [0x25, 0x50, 0x00, 0x03, 0, 0, 0, 0, 0, 0, 0] },
  { name: 'vibration 0.40 amp 0.55 freq 0.55', effect: { type: TriggerType.Vibration, start: 0.4, amplitude: 0.55, frequency: 0.55 },
    bytes: [0x26, 0xf0, 0x03, 0x00, 0xb0, 0x6d, 0x1b, 0, 0, 22, 0] },
  { name: 'slope 0.20-0.80, 0.20 -> 1.00', effect: { type: TriggerType.Slope, start: 0.2, end: 0.8, strength: 0.2, end_strength: 1.0 },
    bytes: [0x21, 0xfc, 0x03, 0x40, 0xb4, 0xfa, 0x3f, 0, 0, 0, 0] },
];

export function runSelfTest() {
  const results = [];
  const check = (name, ok, detail = '') => results.push({ name, ok: !!ok, detail });
  const near = (a, b, eps = 1e-3) => Math.abs(a - b) <= eps;

  check('crc32("123456789") = cbf43926', crc32(new TextEncoder().encode('123456789')) === 0xcbf43926);

  const cal = parseCalibration(syntheticCalibration());
  check('calibration parsed and accepted', cal.valid && near(cal.imu[0].sens, 64) && near(cal.imu[3].sens, 1),
    JSON.stringify(cal.imu.map((c) => [c.bias, +c.sens.toFixed(3)])));

  const usb = parseInputReport(0x01, syntheticUsbPayload(), cal);
  const bit = (b) => (usb.buttons >>> b) & 1;
  check('USB 0x01: buttons', bit(PadButton.South) && bit(PadButton.DpadRight) && bit(PadButton.LeftShoulder) &&
    bit(PadButton.Start) && bit(PadButton.Guide) && !bit(PadButton.East) && !bit(PadButton.DpadUp),
  `0x${usb.buttons.toString(16)}`);
  check('USB 0x01: sticks and triggers', near(usb.left[0], -1) && near(usb.left[1], 1) && Math.abs(usb.right[0]) < 0.01 &&
    near(usb.l2, 1) && near(usb.r2, 0), JSON.stringify([usb.left, usb.right, usb.l2, usb.r2]));
  const gyro64 = (64 * Math.PI) / 180;
  check('USB 0x01: gyro rad/s and accel m/s^2 (calibrated)', near(usb.gyro[0], gyro64) && near(usb.gyro[1], -gyro64) &&
    near(usb.accel[1], 9.80665), JSON.stringify([usb.gyro, usb.accel]));
  check('USB 0x01: two touch points', usb.finger[0].down && near(usb.finger[0].x, 0.5) && near(usb.finger[0].y, 0.5) &&
    !usb.finger[1].down, JSON.stringify(usb.finger));
  check('USB 0x01: battery', near(usb.battery, 0.55) && usb.charging, `${usb.battery} charging=${usb.charging}`);

  const bt = parseInputReport(0x31, syntheticBtPayload(), cal);
  check('Bluetooth 0x31: same state as USB', bt && bt.buttons === usb.buttons && near(bt.left[0], usb.left[0]) &&
    near(bt.gyro[0], usb.gyro[0]) && bt.finger[0].down);
  const corrupt = syntheticBtPayload();
  corrupt[10] ^= 0xff;
  check('Bluetooth 0x31: bad CRC rejected', parseInputReport(0x31, corrupt, cal) === null);

  const simple = parseInputReport(0x01, Uint8Array.of(128, 128, 128, 128, 0x08 | 0x80, 0x02, 0x01, 10, 20), cal);
  check('Bluetooth 0x01 reduced report', simple && !simple.full && ((simple.buttons >>> PadButton.North) & 1) &&
    ((simple.buttons >>> PadButton.RightShoulder) & 1) && ((simple.buttons >>> PadButton.Guide) & 1));

  for (const v of TRIGGER_VECTORS) {
    const got = encodeTriggerEffect(v.effect);
    check(`trigger ${v.name}`, got.length === 11 && got.every((b, i) => b === v.bytes[i]), hex(got));
  }

  const fb = {
    flags: FEEDBACK_FLAGS.rumble | FEEDBACK_FLAGS.led | FEEDBACK_FLAGS.triggers | FEEDBACK_FLAGS.playerLeds,
    rumble: { low: 1, high: 0.5 }, led: [255, 64, 0], playerLeds: 0x04,
    triggers: { left: TRIGGER_VECTORS[1].effect, right: TRIGGER_VECTORS[2].effect },
  };
  const outUsb = buildOutputReport(fb, false);
  check('USB output 0x02 layout', outUsb.reportId === 0x02 && outUsb.data.length === 47 && outUsb.data[0] === 0x0f &&
    outUsb.data[1] === 0x14 && outUsb.data[3] === 255 && outUsb.data[2] === 128 && outUsb.data[10] === 0x25 &&
    outUsb.data[21] === 0x21 && outUsb.data[43] === 0x24 && outUsb.data[44] === 255, hex(outUsb.data));
  const outBt = buildOutputReport(fb, true, 3);
  const stored = (outBt.data[73] | (outBt.data[74] << 8) | (outBt.data[75] << 16) | (outBt.data[76] << 24)) >>> 0;
  check('Bluetooth output 0x31 layout and CRC', outBt.reportId === 0x31 && outBt.data.length === 77 && outBt.data[0] === 0x30 &&
    outBt.data[1] === 0x10 && outBt.data[2 + 10] === 0x25 && stored === bluetoothCrc(0xa2, 0x31, outBt.data),
  `crc ${stored.toString(16)}`);

  const sab = new SharedArrayBuffer(256);
  writePadShared(sab, 0, usb, { bluetooth: true, calibrated: true, epochMs: 1234.5 });
  const u32 = new Uint32Array(sab), f32 = new Float32Array(sab), f64 = new Float64Array(sab);
  check('PadShared layout', u32[0] === 2 && u32[2] === usb.buttons && near(f32[3], -1) && near(f32[7], 1) &&
    u32[PAD_LAYOUT.finger / 4] === 1 && near(f32[PAD_LAYOUT.accel / 4 + 1], 9.80665) && u32[PAD_LAYOUT.reportCount / 4] === 1 &&
    f64[PAD_LAYOUT.epochMs / 8] === 1234.5 && (u32[1] & 0x61) === 0x61);

  const fsab = new SharedArrayBuffer(FEEDBACK_LAYOUT.size);
  const fu32 = new Uint32Array(fsab), ff32 = new Float32Array(fsab), fu8 = new Uint8Array(fsab);
  fu32[0] = 2; fu32[1] = 7; ff32[2] = 0.25; ff32[3] = 0.75; fu8.set([1, 2, 3, 0x1f], 16);
  fu32[FEEDBACK_LAYOUT.right / 4] = TriggerType.Weapon; ff32[FEEDBACK_LAYOUT.right / 4 + 1] = 0.4; ff32[FEEDBACK_LAYOUT.right / 4 + 2] = 0.62;
  const read = readFeedbackShared(fsab, 0);
  check('PadFeedbackShared layout', read && read.flags === 7 && near(read.rumble.high, 0.75) && read.led[2] === 3 &&
    read.playerLeds === 0x1f && read.triggers.right.type === TriggerType.Weapon && near(read.triggers.right.end, 0.62));
  fu32[0] = 3;
  check('PadFeedbackShared: odd sequence means busy', readFeedbackShared(fsab, 0) === null);
  return results;
}
