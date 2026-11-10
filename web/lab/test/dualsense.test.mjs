// node --test web/lab/test/dualsense.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import zlib from 'node:zlib';

import { runSelfTest, syntheticBtPayload } from '../page/dualsense_selftest.js';
import { crc32, buildOutputReport, FEEDBACK_FLAGS } from '../page/dualsense.js';

test('self-test passes', () => {
  for (const r of runSelfTest()) assert.ok(r.ok, `${r.name}: ${r.detail}`);
});

test('crc32 matches zlib', () => {
  const data = new Uint8Array(1000).map((_, i) => (i * 37 + 11) & 0xff);
  assert.equal(crc32(data), zlib.crc32(data));
});

test('Bluetooth input CRC matches zlib over A1 31 payload', () => {
  const p = syntheticBtPayload();
  const expect = zlib.crc32(Buffer.concat([Buffer.from([0xa1, 0x31]), Buffer.from(p.subarray(0, 73))]));
  assert.equal((p[73] | (p[74] << 8) | (p[75] << 16) | (p[76] << 24)) >>> 0, expect);
});

test('Bluetooth output CRC matches zlib over A2 31 payload', () => {
  const { data } = buildOutputReport({ flags: FEEDBACK_FLAGS.led, led: [1, 2, 3] }, true, 5);
  const expect = zlib.crc32(Buffer.concat([Buffer.from([0xa2, 0x31]), Buffer.from(data.subarray(0, 73))]));
  assert.equal((data[73] | (data[74] << 8) | (data[75] << 16) | (data[76] << 24)) >>> 0, expect);
  assert.equal(data[0], 0x50);
});
