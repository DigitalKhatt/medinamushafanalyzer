export const LANDMARK_TYPES = {
  'contour-left': 'Left extent', 'contour-right': 'Right extent',
  'contour-top': 'Top extent', 'contour-bottom': 'Bottom extent',
  'contour-turn': 'Contour turn', 'skeleton-end': 'Stroke endpoint',
  'skeleton-branch': 'Stroke junction',
};

const neighbors = [-1, -1, 0, 1, 1, 1, 0, -1].map((x, i) =>
  [x, [0, -1, -1, -1, 0, 1, 1, 1][i]]);

function thin(mask, width) {
  const ink = mask.slice();
  const offsets = neighbors.map(([x,y]) => y * width + x);
  let changed;
  do {
    changed = false;
    for (const phase of [0, 1]) {
      const remove = [];
      for (let p = width + 1; p < ink.length - width - 1; p++) {
        if (!ink[p]) continue;
        const n = offsets.map(offset => ink[p + offset]);
        const count = n.reduce((sum, value) => sum + value, 0);
        const transitions = n.filter((value, i) => !value && n[(i + 1) % 8]).length;
        // Zhang–Suen thinning, with two directional passes per iteration.
        if (count >= 2 && count <= 6 && transitions === 1 &&
            !(n[2] && n[4] && n[phase ? 0 : 6]) &&
            !(n[6] && n[0] && n[phase ? 2 : 4])) remove.push(p);
      }
      for (const p of remove) ink[p] = 0;
      changed ||= remove.length > 0;
    }
  } while (changed);
  return ink;
}

function traceContours(mask, width) {
  const edges = new Map();
  const add = (x,y,dir) => {
    const p = y * width + x;
    if (!edges.has(p)) edges.set(p, new Set());
    edges.get(p).add(dir);
  };
  for (let p = 0; p < mask.length; p++) {
    if (!mask[p]) continue;
    const x = p % width, y = Math.floor(p / width);
    if (!mask[p-width]) add(x,y,0);
    if (!mask[p+1]) add(x+1,y,1);
    if (!mask[p+width]) add(x+1,y+1,2);
    if (!mask[p-1]) add(x,y+1,3);
  }
  const loops = [], steps = [1, width, -1, -width];
  while (edges.size) {
    const start = edges.keys().next().value, loop = [];
    let p = start, dir = edges.get(p).values().next().value;
    do {
      loop.push([p % width, Math.floor(p / width)]);
      const outgoing = edges.get(p);
      outgoing.delete(dir);
      if (!outgoing.size) edges.delete(p);
      p += steps[dir];
      dir = [1,0,3,2].map(turn => (dir + turn) % 4).find(next => edges.get(p)?.has(next));
    } while (p !== start && dir !== undefined);
    loops.push(loop);
  }
  return loops;
}

export function detectLandmarks(part, pageWidth, allowedInk) {
  const [left, top, w, h] = part.bbox, width = w + 2;
  // Bound interactive thinning for accidental whole-line selections.
  if ((w + 2) * (h + 2) > 40_000) return null;
  const mask = new Uint8Array(width * (h + 2));
  for (let i = 0; i < part.runs.length; i += 2) {
    for (let p = part.runs[i]; p < part.runs[i] + part.runs[i+1]; p++) {
      if (!allowedInk || allowedInk[p]) {
        mask[(Math.floor(p / pageWidth) - top + 1) * width + p % pageWidth - left + 1] = 1;
      }
    }
  }
  const toPage = ([x,y]) => [left + x - 1, top + y - 1];
  const contours = traceContours(mask, width);
  const points = [], skeleton = [], skeletonInk = thin(mask, width);
  const push = (kind, xy) => points.push({kind, x:xy[0], y:xy[1], source:'detected'});
  const boundary = contours.flat();
  for (const [kind, axis, sign] of [['left',0,-1],['right',0,1],['top',1,-1],['bottom',1,1]]) {
    if (!boundary.length) break;
    const extreme = boundary.reduce((best, p) => sign * p[axis] > sign * best[axis] ? p : best);
    const tied = boundary.filter(p => p[axis] === extreme[axis]).sort((a,b) => a[1-axis] - b[1-axis]);
    push('contour-'+kind, toPage(tied[Math.floor((tied.length-1)/2)]));
  }
  const turns = [];
  for (const loop of contours) {
    const n = loop.length;
    if (n < 12) continue;
    const span = Math.max(3, Math.min(7, Math.round(n / 18)));
    loop.forEach((p,i) => {
      const a = loop[(i+n-span)%n], b = loop[(i+span)%n];
      const u = [p[0]-a[0],p[1]-a[1]], v = [b[0]-p[0],b[1]-p[1]];
      const angle = Math.abs(Math.atan2(u[0]*v[1]-u[1]*v[0],u[0]*v[0]+u[1]*v[1]));
      if (angle >= Math.PI * .32) turns.push({p:toPage(p),angle});
    });
  }
  turns.sort((a,b) => b.angle-a.angle);
  for (const {p} of turns) {
    if (points.length >= 20) break;
    if (points.every(q => Math.hypot(p[0]-q.x,p[1]-q.y) >= 4)) push('contour-turn',p);
  }
  for (let p = 0; p < skeletonInk.length; p++) {
    if (!skeletonInk[p]) continue;
    const xy = toPage([p%width+.5, Math.floor(p/width)+.5]);
    skeleton.push(xy);
    const n = neighbors.map(([x,y]) => skeletonInk[p + y*width+x]);
    const branches = n.filter((value,i) => value && !n[(i+1)%8]).length;
    if (n.reduce((sum,value) => sum+value,0) === 1) push('skeleton-end',xy);
    if (branches >= 3 && points.every(q => q.kind !== 'skeleton-branch' ||
        Math.hypot(xy[0]-q.x,xy[1]-q.y) >= 3)) push('skeleton-branch',xy);
  }
  return {contours:contours.map(loop => loop.map(toPage)), skeleton, points};
}

export function sameLandmark(a,b) {
  return a.kind === b.kind && a.x === b.x && a.y === b.y && a.source === b.source;
}
