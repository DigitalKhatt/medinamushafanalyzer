// Page coordinates are raster coordinates. Runs are sorted [start, length, ...].
export const ROLES = [
  {id: 'unknown', label: 'Unlabelled', value: 255, color: '#9aa5b1'},
  {id: 'body', label: 'Letter body', value: 1, color: '#317bc5'},
  {id: 'kashida', label: 'Connection / kashida', value: 2, color: '#dc7d24'},
  {id: 'dot', label: 'Letter dot', value: 3, color: '#9b51c4'},
  {id: 'mark', label: 'Vowel / diacritic', value: 4, color: '#168c79'},
  {id: 'symbol', label: 'Quranic sign / numeral', value: 5, color: '#ca4568'},
  {id: 'ornament', label: 'Ornament / non-text', value: 6, color: '#776554'},
  {id: 'other', label: 'Other ink · pilot negative', value: 7, color: '#687c8b'},
];

export function makeInkMask(rgba, threshold = 170, chroma = 45) {
  const mask = new Uint8Array(rgba.length / 4);
  for (let i = 0; i < mask.length; i++) {
    const j = i * 4, r = rgba[j], g = rgba[j + 1], b = rgba[j + 2];
    // The canvas is composited over white before this function is called.
    mask[i] = rgba[j + 3] > 127 && (r + g + b) / 3 < threshold &&
      Math.max(r, g, b) - Math.min(r, g, b) <= chroma ? 1 : 0;
  }
  return mask;
}

export function runsFromSortedPixels(pixels) {
  const runs = [];
  for (const p of pixels) {
    const n = runs.length;
    if (n && runs[n - 2] + runs[n - 1] === p) runs[n - 1]++;
    else runs.push(p, 1);
  }
  return runs;
}

export function describeRuns(runs, width) {
  let minX = width, minY = Infinity, maxX = -1, maxY = -1, area = 0;
  for (let j = 0; j < runs.length; j += 2) {
    let start = runs[j], remaining = runs[j + 1];
    area += remaining;
    while (remaining) {
      const x = start % width, y = Math.floor(start / width);
      const n = Math.min(remaining, width - x);
      minX = Math.min(minX, x); maxX = Math.max(maxX, x + n - 1);
      minY = Math.min(minY, y); maxY = Math.max(maxY, y);
      start += n; remaining -= n;
    }
  }
  return {area, bbox: area ? [minX, minY, maxX - minX + 1, maxY - minY + 1] : [0, 0, 0, 0]};
}

export function connectedComponents(mask, width, height) {
  if (mask.length !== width * height) throw new Error('Mask dimensions do not match.');
  const labels = new Uint32Array(mask.length);
  const queue = new Uint32Array(mask.length);
  let count = 0;
  // Eight-connected ink; no small-component filtering, so tiny marks are retained.
  for (let i = 0; i < mask.length; i++) {
    if (!mask[i] || labels[i]) continue;
    count++;
    let head = 0, tail = 1; queue[0] = i; labels[i] = count;
    while (head < tail) {
      const p = queue[head++], x = p % width, y = Math.floor(p / width);
      for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
        if ((!dx && !dy) || x + dx < 0 || x + dx >= width || y + dy < 0 || y + dy >= height) continue;
        const n = p + dy * width + dx;
        if (mask[n] && !labels[n]) { labels[n] = count; queue[tail++] = n; }
      }
    }
  }
  const parts = Array.from({length: count}, (_, i) => ({
    id: `part-${String(i + 1).padStart(5, '0')}`, runs: [], role: 'unknown',
    owner: null, line: null, reviewed: false,
  }));
  // A scan in page order produces deterministic, compact run encodings.
  for (let p = 0; p < labels.length;) {
    const label = labels[p];
    if (!label) {p++; continue;}
    const start = p;
    while (p < labels.length && labels[p] === label) p++;
    parts[label - 1].runs.push(start, p - start);
  }
  for (const part of parts) Object.assign(part, describeRuns(part.runs, width));
  return {parts, labels};
}

export function labelMap(parts, width, height) {
  const labels = new Uint32Array(width * height);
  parts.forEach((part, i) => {
    for (let j = 0; j < part.runs.length; j += 2)
      labels.fill(i + 1, part.runs[j], part.runs[j] + part.runs[j + 1]);
  });
  return labels;
}

export function pointInPolygon(x, y, polygon) {
  let inside = false;
  for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i++) {
    const [xi, yi] = polygon[i], [xj, yj] = polygon[j];
    if ((yi > y) !== (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
}

export function splitRuns(runs, width, polygon) {
  if (polygon.length < 3) throw new Error('A split polygon needs at least three points.');
  const inner = [], outer = [];
  let lastInner = -2, lastOuter = -2;
  for (let j = 0; j < runs.length; j += 2) {
    for (let p = runs[j]; p < runs[j] + runs[j + 1]; p++) {
      const isInside = pointInPolygon(p % width + 0.5, Math.floor(p / width) + 0.5, polygon);
      const out = isInside ? inner : outer, last = isInside ? lastInner : lastOuter;
      if (last === p - 1) out[out.length - 1]++;
      else out.push(p, 1);
      if (isInside) lastInner = p; else lastOuter = p;
    }
  }
  return {inner, outer};
}

export function proposeLines(mask, width, height) {
  const rows = new Float64Array(height), smooth = new Float64Array(height);
  for (let p = 0; p < mask.length; p++) if (mask[p]) rows[Math.floor(p / width)]++;
  const radius = Math.max(2, Math.round(height / 450));
  let total = 0;
  for (let y = 0; y < height + radius; y++) {
    if (y < height) total += rows[y];
    if (y > radius * 2) total -= rows[y - radius * 2 - 1];
    if (y >= radius) smooth[y - radius] = total / (radius * 2 + 1);
  }
  const threshold = Math.max(...smooth) * 0.09, bands = [];
  for (let y = 0; y < height;) {
    if (smooth[y] < threshold || !threshold) {y++; continue;}
    const start = y;
    while (y < height && smooth[y] >= threshold) y++;
    const previous = bands[bands.length - 1];
    if (previous && start - previous[1] < radius * 3) previous[1] = y;
    else bands.push([start, y]);
  }
  return bands.map(([top, bottom], i) => ({id: `line-${i + 1}`, top, bottom, reviewed: false}));
}
