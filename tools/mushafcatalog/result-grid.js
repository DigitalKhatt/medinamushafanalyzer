// Reserve the entire result set's scroll range, but keep only nearby cards in
// the DOM. Offsets are random-access: reaching the end never loads prior rows.
const MAX_HEIGHT = 8_000_000; // Below browser layout limits, including on mobile.
const clamp = (n, max) => Math.max(0, Math.min(n, max));

export function gridLayout({count, width, minWidth, gap, cardHeight, viewport, scrollTop}) {
  const columns = Math.max(1, Math.floor((width + gap) / (minWidth + gap)));
  const pitch = cardHeight + gap, rows = Math.ceil(count / columns);
  const logicalHeight = Math.max(0, rows * pitch - gap);
  const height = Math.min(MAX_HEIGHT, logicalHeight);
  const top = clamp(scrollTop, height - viewport);
  const logicalTop = height > viewport ? top * Math.max(0, logicalHeight - viewport) / (height - viewport) : 0;
  const first = Math.max(0, Math.floor(logicalTop / pitch) - 2);
  const last = Math.min(rows, Math.ceil((logicalTop + viewport) / pitch) + 2);
  return {columns, pitch, height, logicalHeight, top, logicalTop, viewport,
    start: first * columns, end: Math.min(count, last * columns),
    translate: first * pitch - logicalTop + top};
}

export function scrollPosition(layout, index, alignEnd = false) {
  const {columns, pitch, viewport, logicalHeight, height} = layout;
  const max = Math.max(0, logicalHeight - viewport);
  const top = clamp(Math.floor(index / columns) * pitch - (alignEnd ? viewport - pitch : 0), max);
  return max ? top / max * Math.max(0, height - viewport) : 0;
}

export class ResultGrid {
  constructor(container, stage, {item, card, range, painted}) {
    Object.assign(this, {container, stage, item, card, range, painted, count: 0, nodes: new Map()});
    this.window = document.createElement('div');
    this.window.className = 'grid result-window';
    container.replaceChildren(this.window);
    this.schedule = () => {
      if (!this.frame) this.frame = requestAnimationFrame(() => { this.frame = null; this.paint(); });
    };
    this.keydown = event => this.onKey(event);
    this.resize = new ResizeObserver(this.schedule);
    this.resize.observe(container);
    this.resize.observe(stage);
    stage.addEventListener('scroll', this.schedule, {passive: true});
    window.addEventListener('scroll', this.schedule, {passive: true});
    window.addEventListener('resize', this.schedule);
    stage.addEventListener('keydown', this.keydown);
  }
  destroy() {
    cancelAnimationFrame(this.frame);
    this.resize.disconnect();
    this.stage.removeEventListener('scroll', this.schedule);
    window.removeEventListener('scroll', this.schedule);
    window.removeEventListener('resize', this.schedule);
    this.stage.removeEventListener('keydown', this.keydown);
    this.container.replaceChildren();
    this.container.style.height = '';
  }
  setCount(count) { this.count = count; this.schedule(); }
  refresh(redraw = false) {
    if (redraw) for (const node of this.nodes.values()) node.stale = true;
    this.schedule();
  }
  geometry() {
    this.mobile = matchMedia('(max-width:760px)').matches;
    const css = getComputedStyle(this.container), rect = this.container.getBoundingClientRect();
    const origin = this.mobile ? 0 : this.stage.getBoundingClientRect().top + this.stage.clientTop;
    return gridLayout({count: this.count, width: rect.width,
      minWidth: parseFloat(css.getPropertyValue('--card-width')),
      cardHeight: parseFloat(css.getPropertyValue('--card-height')),
      gap: parseFloat(css.getPropertyValue('--card-gap')),
      viewport: this.mobile ? innerHeight : this.stage.clientHeight, scrollTop: origin - rect.top});
  }
  scrollTo(index, alignEnd = false) {
    if (!this.layout) return;
    const target = scrollPosition(this.layout, index, alignEnd);
    const rect = this.container.getBoundingClientRect();
    const origin = this.mobile ? 0 : this.stage.getBoundingClientRect().top + this.stage.clientTop;
    const delta = target + rect.top - origin;
    if (this.mobile) window.scrollBy(0, delta);
    else this.stage.scrollTop += delta;
    this.schedule();
  }
  onKey(event) {
    if (!this.count || event.altKey || event.metaKey || event.ctrlKey) return;
    const current = event.target.closest('[data-grid-index]');
    let target;
    if (event.key === 'Home') target = 0;
    else if (event.key === 'End') target = this.count - 1;
    else if (current) {
      const step = {ArrowLeft: -1, ArrowRight: 1, ArrowUp: -this.layout.columns, ArrowDown: this.layout.columns}[event.key];
      if (step) target = clamp(Number(current.dataset.gridIndex) + step, this.count - 1);
    }
    if (target === undefined) return;
    event.preventDefault();
    this.focusIndex = target;
    this.scrollTo(target, event.key === 'End');
  }
  paint() {
    const previous = this.layout;
    this.layout = this.geometry();
    this.container.style.height = this.layout.height + 'px';
    if (previous && previous.end && (previous.columns !== this.layout.columns || previous.pitch !== this.layout.pitch)) {
      // Keep the same result in view when the sidebar, viewport or pair option
      // changes the number of columns or the card height.
      const atEnd = previous.top >= previous.height - previous.viewport - 2;
      const anchor = atEnd ? this.count - 1 : Math.floor(previous.logicalTop / previous.pitch) * previous.columns;
      this.scrollTo(Math.max(0, anchor), atEnd);
      this.layout = this.geometry();
    }
    const {columns, start, end, translate} = this.layout;
    this.window.style.gridTemplateColumns = `repeat(${columns}, minmax(0, 1fr))`;
    this.window.style.transform = `translateY(${translate}px)`;
    const active = document.activeElement;
    for (const [index, entry] of this.nodes) if (index < start || index >= end) {
      if (entry.node.contains(active)) this.stage.focus({preventScroll: true});
      entry.node.remove(); this.nodes.delete(index);
    }
    let cursor = this.window.firstChild;
    for (let index = start; index < end; index++) {
      const value = this.item(index), entry = this.nodes.get(index);
      let node = entry?.node;
      if (!entry || entry.value !== value || entry.stale) {
        node = this.card(value, index);
        node.dataset.gridIndex = index;
        if (entry) {
          const focused = entry.node === active;
          entry.node.replaceWith(node);
          if (cursor === entry.node) cursor = node;
          if (focused) node.focus({preventScroll: true});
        }
        this.nodes.set(index, {node, value});
      }
      if (cursor !== node) this.window.insertBefore(node, cursor);
      cursor = node.nextSibling;
      if (this.focusIndex === index && node.dataset.focus) {
        node.focus({preventScroll: true}); this.focusIndex = null;
      }
    }
    const rangeKey = start + ':' + end;
    if (rangeKey !== this.rangeKey) {
      this.rangeKey = rangeKey;
      this.range(start, end);
    }
    this.painted();
  }
}
