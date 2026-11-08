import test from 'node:test';
import assert from 'node:assert/strict';
import {detectLandmarks} from '../app/landmarks.mjs';

function shape(rows) {
  const width = rows[0].length;
  const runs = rows.flatMap((row, y) => [...row].flatMap((ink, x) =>
    ink === '#' ? [y * width + x, 1] : []));
  return [{bbox:[0,0,width,rows.length],runs}, width];
}

test('a ring retains outer and inner contour and has no stroke ends', () => {
  const ring = shape(['#######','#######','##...##','##...##','##...##','#######','#######']);
  const result = detectLandmarks(...ring);
  assert.equal(result.contours.length,2);
  assert.ok(result.skeleton.length > 0);
  assert.equal(result.points.filter(p => p.kind === 'skeleton-end').length,0);
  assert.deepEqual(detectLandmarks(...ring),result);
});

test('a fork has a junction and three endpoints', () => {
  const fork = shape(['#######','...#...','...#...','...#...','...#...']);
  const {points} = detectLandmarks(...fork);
  assert.equal(points.filter(p => p.kind === 'skeleton-end').length,3);
  assert.equal(points.filter(p => p.kind === 'skeleton-branch').length,1);
});
