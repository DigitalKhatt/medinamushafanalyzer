const byId = (id) => document.getElementById(id);
const sheet = byId('sheet'), viewport = byId('viewport');
const tooltip = byId('glyph-tooltip');
const pageSources = new Map();
const RECENT_PAGES = 8;
let pages = [], current = 0, revision = 0, hoveredGroup = null, hoveredMark = null;
let joins = [], standaloneLayer = null;
function paint() {
  const colored = byId('palette').checked;
  for (const group of sheet.querySelectorAll('.letter, .kashida')) {
    group.setAttribute('fill', colored ? group.dataset.palette : '#231f20');
  }
}
function describe(group) {
  byId('selection').textContent = group ? [group.dataset.text, group.dataset.owner, group.dataset.quality].filter(Boolean).join(' · ') :
    'Hover over the ink to inspect it.';
}
function hideTooltip() {
  if (tooltip.hidden) return;
  tooltip.hidden = true;
  hoveredGroup = null;
  hoveredMark = null;
  describe(null);
}
function letterPreview(group, mark) {
  const {x, y, width, height} = group.getBBox();
  const pad = Math.max(width, height) * .06;
  const svg = document.createElementNS(group.namespaceURI, 'svg');
  svg.classList.add('tooltip-letter');
  svg.setAttribute('viewBox', [x - pad, y - pad, width + 2 * pad, height + 2 * pad].join(' '));
  svg.setAttribute('aria-hidden', 'true');
  const drawing = group.cloneNode(true);
  drawing.removeAttribute('class');
  drawing.setAttribute('fill', '#231f20');
  if (mark) {
    for (const path of drawing.querySelectorAll('[data-mark]')) {
      if (path.dataset.mark === mark) path.setAttribute('fill', '#9b431f');
    }
  }
  svg.append(drawing);
  return svg;
}
function showTooltip(event) {
  const group = event.target.closest('.letter, .waqf, .kashida');
  if (!group || event.pointerType === 'touch') {
    if (!tooltip.hidden) hideTooltip();
    return;
  }
  const markShape = event.target.closest('[data-mark]');
  const mark = markShape?.dataset.mark || null;
  const {character, characterName, dotLabel} = (markShape || group).dataset;
  const standalone = group.classList.contains('kashida');
  if (!character && !standalone) {
    hideTooltip();
    return;
  }
  if (tooltip.hidden || hoveredGroup !== group || hoveredMark !== mark) {
    const complete = document.createElement('span');
    complete.className = 'tooltip-complete';
    complete.dir = standalone ? 'ltr' : 'rtl';
    complete.textContent = group.dataset.text;

    const details = document.createElement('span');
    details.className = 'tooltip-character';
    details.dir = 'rtl';
    details.textContent = character;
    tooltip.replaceChildren(letterPreview(group, mark), complete);
    if (character) tooltip.append(details);
    if (dotLabel || characterName) {
      const name = document.createElement('span');
      name.className = 'unicode-name';
      name.dir = 'ltr';
      name.lang = 'en';
      name.textContent = dotLabel || characterName;
      tooltip.append(name);
    }
    tooltip.hidden = false;
    hoveredMark = mark;
  }
  if (hoveredGroup !== group) {
    describe(group);
    hoveredGroup = group;
  }
  const gap = 12;
  const left = Math.max(8, Math.min(event.clientX + gap, innerWidth - tooltip.offsetWidth - 8));
  const below = event.clientY + gap;
  const top = below + tooltip.offsetHeight <= innerHeight - 8 ? below : event.clientY - tooltip.offsetHeight - gap;
  tooltip.style.left = left + 'px';
  tooltip.style.top = Math.max(8, top) + 'px';
}
function prepareKashidas(svg) {
  const byOutline = new Map();
  for (const part of svg.querySelectorAll('use[data-part="join-half"]')) {
    const outline = part.getAttribute('href');
    const clip = part.getAttribute('clip-path');
    // The middle SVG uses the a clip for the right letter and b for the left.
    const side = clip.endsWith('a)') ? 'right' : 'left';
    if (!byOutline.has(outline)) byOutline.set(outline, {});
    byOutline.get(outline)[side] = {part, clip};
  }
  joins = [...byOutline.values()].filter(join => join.left && join.right);
  standaloneLayer = null;
}

function applyKashidaMode() {
  hideTooltip();
  const mode = byId('kashida').value;
  for (const join of joins) {
    for (const side of ['left', 'right']) {
      const {part, clip} = join[side];
      part.style.display = mode === 'middle' || mode === side ? '' : 'none';
      if (mode === 'middle') part.setAttribute('clip-path', clip);
      else part.removeAttribute('clip-path');
    }
  }
  if (mode === 'standalone' && joins.length && !standaloneLayer) {
    const first = joins[0].right.part;
    standaloneLayer = document.createElementNS(first.namespaceURI, 'g');
    for (const {right, left} of joins) {
      const group = document.createElementNS(first.namespaceURI, 'g');
      group.setAttribute('class', 'kashida');
      group.dataset.text = 'Kashida';
      group.dataset.owner = right.part.parentNode.dataset.owner + ' → ' + left.part.parentNode.dataset.owner;
      group.dataset.palette = '#52627b';
      const whole = document.createElementNS(first.namespaceURI, 'use');
      whole.setAttribute('href', right.part.getAttribute('href'));
      whole.dataset.part = 'kashida';
      group.append(whole);
      standaloneLayer.append(group);
    }
    // Keep standalone ink inside the same frame clip as the letter groups.
    first.closest('.word').parentNode.append(standaloneLayer);
  }
  if (standaloneLayer) standaloneLayer.style.display = mode === 'standalone' ? '' : 'none';
  paint();
}

function pageSource(page) {
  const cached = pageSources.get(page);
  if (cached) {
    pageSources.delete(page);
    pageSources.set(page, cached);
    return cached;
  }
  const params = new URLSearchParams({page, interior: '1', kashida: 'middle'});
  const request = fetch('/api/page.svg?' + params).then(async response => {
    if (!response.ok) {
      const error = new DOMParser().parseFromString(await response.text(), 'text/html');
      throw new Error(error.querySelectorAll('p')[1]?.textContent || 'Could not load page ' + page + '.');
    }
    return response.text();
  }).catch(error => {
    if (pageSources.get(page) === request) pageSources.delete(page);
    throw error;
  });
  pageSources.set(page, request);
  if (pageSources.size > RECENT_PAGES) pageSources.delete(pageSources.keys().next().value);
  return request;
}

async function load(page) {
  const ticket = ++revision;
  current = page;
  byId('number').value = page;
  byId('prev').disabled = !pages.some(p => p < page);
  byId('next').disabled = !pages.some(p => p > page);
  byId('summary').textContent = 'Loading page ' + page + '…';
  sheet.replaceChildren();
  joins = [];
  standaloneLayer = null;
  hideTooltip();
  describe(null);
  try {
    if (!pages.includes(page)) throw new Error('Page ' + page + ' is outside this catalog.');
    const source = await pageSource(page);
    if (ticket !== revision) return;
    const doc = new DOMParser().parseFromString(source, 'image/svg+xml');
    const svg = doc.documentElement;
    if (svg.localName !== 'svg') throw new Error('Invalid page SVG.');
    svg.setAttribute('aria-label', svg.querySelector('title')?.textContent || 'Mushaf page');
    svg.querySelectorAll('title').forEach(title => title.remove());
    const drawing = document.importNode(svg, true);
    sheet.replaceChildren(drawing);
    prepareKashidas(drawing);
    applyKashidaMode();
    viewport.scrollTo(0, 0);
    const pending = Number(svg.dataset.pending);
    const pendingMarks = Number(svg.dataset.pendingMarks);
    byId('summary').textContent = 'Page ' + page + ' · ' + svg.dataset.letters + ' letters' +
      (pending ? ' · ' + pending + ' awaiting verified body vectors' : ' · all bodies segmented') +
      (pendingMarks ? ' · ' + pendingMarks + ' marks remain in the source artwork' : '');
  } catch (error) {
    if (ticket === revision) byId('summary').textContent = error.message;
  }
}
function go(page) {
  if (location.hash === '#' + page) load(page);
  else location.hash = String(page);
}
function step(direction) {
  const next = direction > 0 ? pages.find(p => p > current) : pages.findLast(p => p < current);
  if (next) go(next);
}
byId('number').onchange = () => go(Number(byId('number').value));
byId('prev').onclick = () => step(-1);
byId('next').onclick = () => step(1);
byId('palette').onchange = paint;
byId('kashida').onchange = applyKashidaMode;
byId('zoom').oninput = () => sheet.style.setProperty('--zoom', Number(byId('zoom').value) / 100);
sheet.onpointerover = sheet.onpointermove = showTooltip;
sheet.onpointerleave = hideTooltip;
viewport.onscroll = hideTooltip;
document.addEventListener('keydown', e => {
  if (e.target.closest('input, select, textarea') || e.altKey || e.ctrlKey || e.metaKey) return;
  if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
    e.preventDefault(); step(e.key === 'ArrowLeft' ? 1 : -1);
  }
});
function requestedPage() {
  return location.hash ? Number(location.hash.slice(1)) : pages[0];
}
try {
  const response = await fetch('/api/viewer-pages');
  if (!response.ok) throw new Error('Could not read the catalog page list.');
  pages = await response.json();
  if (!pages.length) throw new Error('This catalog has no pages.');
  window.onhashchange = () => load(requestedPage());
  await load(requestedPage());
} catch (error) { byId('summary').textContent = error.message; }
