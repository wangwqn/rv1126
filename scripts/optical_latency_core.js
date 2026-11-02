(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.OpticalLatency = api;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  const DIGIT_SEGMENTS = {
    "0": "abcdef", "1": "bc", "2": "abdeg", "3": "abcdg", "4": "bcfg",
    "5": "acdfg", "6": "acdefg", "7": "abc", "8": "abcdefg", "9": "abcdfg"
  };
  // Canonical ordering makes comparisons independent of the strings above.
  const SEGMENT_ORDER = "abcdefg";
  const SEGMENT_RECTS = {
    a: [0.27, 0.06, 0.73, 0.14],
    b: [0.75, 0.17, 0.86, 0.44],
    c: [0.75, 0.56, 0.86, 0.83],
    d: [0.27, 0.86, 0.73, 0.94],
    e: [0.14, 0.56, 0.25, 0.83],
    f: [0.14, 0.17, 0.25, 0.44],
    g: [0.27, 0.46, 0.73, 0.54]
  };
  const LAYOUT = { left: 0.02, right: 0.98, top: 0.03, bottom: 0.97, gap: 0.008, digits: 9 };

  function canonicalSegments(value) {
    return SEGMENT_ORDER.split("").filter((s) => value.includes(s)).join("");
  }

  const PATTERN_TO_DIGIT = {};
  Object.keys(DIGIT_SEGMENTS).forEach((digit) => {
    PATTERN_TO_DIGIT[canonicalSegments(DIGIT_SEGMENTS[digit])] = digit;
  });

  function digitBox(index, width, height) {
    const usable = LAYOUT.right - LAYOUT.left - LAYOUT.gap * (LAYOUT.digits - 1);
    const cell = usable / LAYOUT.digits;
    return {
      x: (LAYOUT.left + index * (cell + LAYOUT.gap)) * width,
      y: LAYOUT.top * height,
      width: cell * width,
      height: (LAYOUT.bottom - LAYOUT.top) * height
    };
  }

  function segmentBox(index, segment, width, height) {
    const cell = digitBox(index, width, height);
    const r = SEGMENT_RECTS[segment];
    return {
      x: cell.x + r[0] * cell.width,
      y: cell.y + r[1] * cell.height,
      width: (r[2] - r[0]) * cell.width,
      height: (r[3] - r[1]) * cell.height
    };
  }

  function formatTimecode(date) {
    const pad = (value, width) => String(value).padStart(width, "0");
    return pad(date.getHours(), 2) + pad(date.getMinutes(), 2) +
      pad(date.getSeconds(), 2) + pad(date.getMilliseconds(), 3);
  }

  function timecodeToMs(value) {
    if (!/^\d{9}$/.test(value)) throw new Error("timecode must contain exactly 9 digits");
    const hour = Number(value.slice(0, 2));
    const minute = Number(value.slice(2, 4));
    const second = Number(value.slice(4, 6));
    const millisecond = Number(value.slice(6, 9));
    if (hour > 23 || minute > 59 || second > 59) throw new Error("invalid timecode " + value);
    return ((hour * 60 + minute) * 60 + second) * 1000 + millisecond;
  }

  function latencyMs(source, streamed) {
    const day = 24 * 60 * 60 * 1000;
    return (timecodeToMs(source) - timecodeToMs(streamed) + day) % day;
  }

  function percentile(values, p) {
    if (!values.length) return null;
    const sorted = values.slice().sort((a, b) => a - b);
    return sorted[Math.max(0, Math.ceil(p * sorted.length) - 1)];
  }

  function drawTimecode(ctx, value, width, height) {
    if (!/^\d{9}$/.test(value)) throw new Error("timecode must contain exactly 9 digits");
    ctx.fillStyle = "#020607";
    ctx.fillRect(0, 0, width, height);
    ctx.strokeStyle = "#ffffff";
    ctx.lineWidth = Math.max(2, height * 0.012);
    ctx.strokeRect(width * 0.008, height * 0.015, width * 0.984, height * 0.97);
    for (let index = 0; index < value.length; ++index) {
      for (const segment of SEGMENT_ORDER) {
        const box = segmentBox(index, segment, width, height);
        ctx.fillStyle = DIGIT_SEGMENTS[value[index]].includes(segment) ? "#f5fff8" : "#081411";
        ctx.fillRect(box.x, box.y, box.width, box.height);
      }
    }
  }

  function regionMean(image, box) {
    const x0 = Math.max(0, Math.floor(box.x));
    const y0 = Math.max(0, Math.floor(box.y));
    const x1 = Math.min(image.width, Math.ceil(box.x + box.width));
    const y1 = Math.min(image.height, Math.ceil(box.y + box.height));
    let total = 0;
    let count = 0;
    for (let y = y0; y < y1; ++y) {
      for (let x = x0; x < x1; ++x) {
        const offset = (y * image.width + x) * 4;
        total += image.data[offset] * 0.2126 + image.data[offset + 1] * 0.7152 +
          image.data[offset + 2] * 0.0722;
        ++count;
      }
    }
    return count ? total / count : 0;
  }

  function decodeTimecode(image) {
    if (!image || !image.data || !image.width || !image.height)
      throw new Error("invalid RGBA image");
    const means = [];
    const backgroundMeans = [];
    const samples = [];
    for (let index = 0; index < LAYOUT.digits; ++index) {
      const digit = {};
      for (const segment of SEGMENT_ORDER) {
        const mean = regionMean(image, segmentBox(index, segment, image.width, image.height));
        digit[segment] = mean;
        means.push(mean);
      }
      const cell = digitBox(index, image.width, image.height);
      backgroundMeans.push(regionMean(image, {
        x: cell.x + cell.width * 0.40,
        y: cell.y + cell.height * 0.25,
        width: cell.width * 0.20,
        height: cell.height * 0.12
      }));
      samples.push(digit);
    }
    const sorted = means.slice().sort((a, b) => a - b);
    const dark = percentile(backgroundMeans, 0.50);
    const bright = percentile(sorted, 0.90);
    if (bright - dark < 30) throw new Error("timecode contrast is too low");
    const threshold = (dark + bright) / 2;
    let value = "";
    let minimumMargin = 1;
    for (const digit of samples) {
      const pattern = SEGMENT_ORDER.split("").filter((s) => digit[s] >= threshold).join("");
      if (!(pattern in PATTERN_TO_DIGIT)) throw new Error("unknown segment pattern " + pattern);
      value += PATTERN_TO_DIGIT[pattern];
      for (const segment of SEGMENT_ORDER) {
        minimumMargin = Math.min(minimumMargin, Math.abs(digit[segment] - threshold) /
          Math.max(1, bright - dark));
      }
    }
    // Validate hour/minute/second ranges before returning a seemingly valid result.
    timecodeToMs(value);
    return { value, confidence: Math.max(0, Math.min(1, minimumMargin * 2)), threshold, dark, bright };
  }

  return {
    DIGIT_SEGMENTS, SEGMENT_ORDER, SEGMENT_RECTS, LAYOUT, digitBox, segmentBox,
    formatTimecode, timecodeToMs, latencyMs, percentile, drawTimecode, decodeTimecode
  };
});
