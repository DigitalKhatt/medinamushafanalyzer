import assert from 'node:assert/strict';
import test from 'node:test';
import {gridLayout, scrollPosition} from './result-grid.js';

const desktop = {width: 1072, minWidth: 140, cardHeight: 256, gap: 10, viewport: 680};
const mobile = {width: 372, minWidth: 125, cardHeight: 284, gap: 10, viewport: 820};

test('the full range exists immediately, including a partially filled last row', () => {
  const first = gridLayout({...desktop, count: 1003, scrollTop: 0});
  const last = gridLayout({...desktop, count: 1003, scrollTop: first.height});
  assert.equal(first.columns, 7);
  assert.equal(first.height, Math.ceil(1003 / 7) * 266 - 10);
  assert.equal(last.end, 1003);
  assert.ok(last.start > 900);
  assert.ok(last.end - last.start < 70);
});

test('hundreds of thousands of mobile cards stay below browser height limits', () => {
  const count = 474986;
  const first = gridLayout({...mobile, count, scrollTop: 0});
  assert.ok(first.logicalHeight > 33_000_000);
  assert.ok(first.height <= 8_000_000);
  const last = gridLayout({...mobile, count, scrollTop: first.height});
  assert.equal(last.end, count);
  assert.ok(last.end - last.start < 20);
  // The final card ends at the end of the reserved scroll area, not below it.
  const renderedHeight = Math.ceil((last.end - last.start) / last.columns) * last.pitch - mobile.gap;
  assert.ok(Math.abs(last.translate + renderedHeight - last.height) < 0.001);
});

test('direct jumps cover every target without loading intervening ranges', () => {
  for (const dimensions of [desktop, mobile]) {
    for (const count of [0, 1, 24, 400, 1000, 10001, 149133, 438724, 474986]) {
      const first = gridLayout({...dimensions, count, scrollTop: 0});
      for (let index = 0; index < count; index += Math.max(1, Math.floor(count / 100))) {
        const position = scrollPosition(first, index);
        const window = gridLayout({...dimensions, count, scrollTop: position});
        assert.ok(window.start <= index && index < window.end, `${count}: missing ${index}`);
        assert.ok(window.end - window.start < 100);
        assert.ok(window.translate <= window.top);
      }
      if (count) {
        const end = gridLayout({...dimensions, count, scrollTop: scrollPosition(first, count - 1, true)});
        assert.equal(end.end, count);
      }
    }
  }
});

test('empty and small result sets have no phantom rows', () => {
  for (const count of [0, 1, 6]) {
    const layout = gridLayout({...desktop, count, scrollTop: 9999});
    assert.equal(layout.start, 0);
    assert.equal(layout.end, count);
    assert.equal(layout.top, 0);
    assert.equal(scrollPosition(layout, 0), 0);
  }
});
