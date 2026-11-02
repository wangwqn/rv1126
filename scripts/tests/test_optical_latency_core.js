"use strict";
const assert = require("assert");
const optical = require("../optical_latency_core.js");

function syntheticTimecode(value, width = 1350, height = 300) {
  const data = new Uint8ClampedArray(width * height * 4);
  for (let i = 0; i < data.length; i += 4) {
    data[i] = 2; data[i + 1] = 6; data[i + 2] = 7; data[i + 3] = 255;
  }
  const paint = (box, lit) => {
    const rgb = lit ? [245, 255, 248] : [8, 20, 17];
    for (let y = Math.floor(box.y); y < Math.ceil(box.y + box.height); ++y) {
      for (let x = Math.floor(box.x); x < Math.ceil(box.x + box.width); ++x) {
        if (x < 0 || y < 0 || x >= width || y >= height) continue;
        const offset = (y * width + x) * 4;
        const noise = ((x * 17 + y * 31) % 9) - 4;
        data[offset] = Math.max(0, Math.min(255, rgb[0] + noise));
        data[offset + 1] = Math.max(0, Math.min(255, rgb[1] + noise));
        data[offset + 2] = Math.max(0, Math.min(255, rgb[2] + noise));
      }
    }
  };
  for (let index = 0; index < value.length; ++index) {
    for (const segment of optical.SEGMENT_ORDER) {
      paint(optical.segmentBox(index, segment, width, height),
        optical.DIGIT_SEGMENTS[value[index]].includes(segment));
    }
  }
  return {data, width, height};
}

const cases = ["000000000", "123456789", "235959999", "080102003"];
for (const value of cases) {
  const result = optical.decodeTimecode(syntheticTimecode(value));
  assert.strictEqual(result.value, value);
  assert(result.confidence > 0.8);
}

assert.strictEqual(optical.latencyMs("120000250", "120000100"), 150);
assert.strictEqual(optical.latencyMs("000000050", "235959900"), 150);
assert.strictEqual(optical.percentile([5, 1, 2, 3, 4], 0.50), 3);
assert.strictEqual(optical.percentile([5, 1, 2, 3, 4], 0.95), 5);
assert.throws(() => optical.timecodeToMs("246000000"), /invalid/);
console.log("optical latency core tests passed");
