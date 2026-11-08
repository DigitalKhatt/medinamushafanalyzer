import test from 'node:test';
import assert from 'node:assert/strict';
import {makeInkMask, connectedComponents, labelMap, splitRuns, describeRuns, proposeLines} from '../app/core.mjs';

test('ink extraction rejects green ornament and retains dark neutral ink', () => {
  const rgba = new Uint8ClampedArray([35,31,32,255, 191,232,193,255, 255,255,255,255, 0,0,0,0]);
  assert.deepEqual([...makeInkMask(rgba)], [1,0,0,0]);
});

test('eight-connectivity retains a tiny isolated dot and never wraps across a row', () => {
  const mask = new Uint8Array([
    1,0,0,0,1,
    0,1,0,0,0,
    0,0,0,0,0,
    1,0,0,0,0,
  ]);
  const {parts} = connectedComponents(mask, 5, 4);
  assert.equal(parts.length, 3);
  assert.deepEqual(parts.map(p => p.area), [2,1,1]);
  assert.deepEqual([...labelMap(parts,5,4)].map(x => x ? 1 : 0), [...mask]);
  const edge = connectedComponents(new Uint8Array([0,0,1,1,0,0]), 3, 2);
  assert.equal(edge.parts.length, 2);
});

test('a counter stays empty after component extraction and a body/connection split', () => {
  const mask = new Uint8Array([
    1,1,1,1,1,
    1,0,0,0,1,
    1,0,0,0,1,
    1,1,1,1,1,
  ]);
  const {parts} = connectedComponents(mask,5,4);
  assert.equal(parts.length,1);
  const {inner,outer} = splitRuns(parts[0].runs,5,[[0,0],[2.5,0],[2.5,4],[0,4]]);
  const labels = labelMap([{runs:inner},{runs:outer}],5,4);
  assert.deepEqual([...labels].map(x => x ? 1 : 0),[...mask]);
  assert.equal(labels[6],0);
  assert.equal(describeRuns(inner,5).area + describeRuns(outer,5).area,14);
});

test('runs crossing a row boundary have an exact bounding box', () => {
  assert.deepEqual(describeRuns([3,4],5), {area:4,bbox:[0,0,5,2]});
});

test('randomized masks preserve every pixel through extraction and polygon split', () => {
  let seed = 23;
  const random = () => ((seed = (1664525 * seed + 1013904223) >>> 0) / 2**32);
  for (let iteration = 0; iteration < 120; iteration++) {
    const width = 1 + Math.floor(random()*35), height = 1 + Math.floor(random()*35);
    const mask = Uint8Array.from({length:width*height}, () => random() < .4 ? 1 : 0);
    const {parts} = connectedComponents(mask,width,height);
    const splits = parts.flatMap(part => {
      const {inner,outer} = splitRuns(part.runs,width,[[0,0],[width*.53,0],[width*.53,height],[0,height]]);
      const a = new Set();
      for (let i=0;i<inner.length;i+=2) for(let p=inner[i];p<inner[i]+inner[i+1];p++) a.add(p);
      for (let i=0;i<outer.length;i+=2) for(let p=outer[i];p<outer[i]+outer[i+1];p++) assert.equal(a.has(p),false);
      return [inner,outer].filter(runs=>runs.length).map(runs=>({runs}));
    });
    assert.deepEqual([...labelMap(splits,width,height)].map(x=>x?1:0),[...mask]);
  }
});

test('line proposals remain unreviewed and do not assign ownership', () => {
  const mask = new Uint8Array(100*100);
  for(const y of [10,11,12,50,51,52]) mask.fill(1,y*100+10,y*100+90);
  const lines = proposeLines(mask,100,100);
  assert.equal(lines.length,2);
  assert.ok(lines.every(line=>line.reviewed===false));
});
