const el = id => document.getElementById(id);
const fmt = n => n.toLocaleString('en-US');
const sourceName = {'human-reviewed': 'Human-reviewed', 'machine-weak': 'Automatic template'};
const pageSize = 10;
let gallery, matches, selected, page = 0;

function normal(value) {
  return value.normalize('NFD').replace(/[\p{M}ـ]/gu, '').toLowerCase().trim();
}

function pageQuery(value) {
  const match = /^(?:page\s*)?(\d{1,3})$/i.exec(value);
  return match && 'page' + match[1].padStart(3, '0');
}

function urlFor(row, draw = '') {
  const url = new URL(location.href);
  for (const [key, value] of Object.entries({source: el('source').value, q: el('search').value.trim(), draw})) {
    if (value && value !== 'all') url.searchParams.set(key, value);
    else url.searchParams.delete(key);
  }
  url.hash = row?.id || '';
  return url;
}

function populateList() {
  const start = page * pageSize;
  el('list').replaceChildren();
  const end = Math.min(matches.length, start + pageSize);
  el('range').textContent = matches.length ? `${fmt(start + 1)}–${fmt(end)} of ${fmt(matches.length)}` : '0 examples';
  el('back').disabled = start === 0;
  el('forward').disabled = end === matches.length;
  for (const row of matches.slice(start, end)) {
    const button = document.createElement('button');
    button.className = 'entry';
    button.setAttribute('aria-current', String(row === selected));
    const image = document.createElement('img');
    image.alt = '';
    image.src = `base/${row.file}.png`;
    const title = document.createElement('span');
    const text = document.createElement('b');
    text.lang = 'ar'; text.dir = 'rtl'; text.className = 'arabic'; text.textContent = row.text;
    const id = document.createElement('small');
    id.textContent = row.id;
    title.append(text, id); button.append(image, title);
    button.addEventListener('click', () => show(row));
    el('list').append(button);
  }
}

function updateView() {
  const value = el('view').value;
  const draw = selected.draws.find(([step, slot]) => value === `${step}-${slot}`);
  const file = draw ? `draws/${value}.png` : `base/${selected.file}.png`;
  el('ink').src = el('target').src = el('png').href = file;
  const [bodies, kashidas] = draw ? draw.slice(2) : [selected.bodies, selected.kashidas];
  el('bodies').textContent = `${bodies} letter bod${bodies === 1 ? 'y' : 'ies'}`;
  el('kashidas').textContent = `${kashidas} kashida${kashidas === 1 ? '' : 's'}`;
  history.replaceState(null, '', urlFor(selected, value === el('view').options[0].value ? '' : value));
}

function show(row, preferredDraw) {
  selected = row;
  el('detail').hidden = !row;
  el('empty').hidden = Boolean(row);
  if (row) {
    page = Math.floor(matches.indexOf(row) / pageSize);
    el('identity').textContent = row.id;
    el('word').textContent = row.text;
    el('meta').textContent = `${sourceName[row.source]} · sampled ${row.draws.length} time${row.draws.length === 1 ? '' : 's'}`;
    el('templates').hidden = !row.templates.length;
    el('templates').textContent = 'Source templates · ' + row.templates.join(', ');
    el('raw').href = `raw/${row.file}.npz`;
    el('view').replaceChildren(...row.draws.map(([step, slot]) =>
      new Option(`Step ${fmt(step)} · batch item ${slot}`, `${step}-${slot}`)));
    el('view').add(new Option('Before augmentation', 'base'));
    if ([...el('view').options].some(o => o.value === preferredDraw)) el('view').value = preferredDraw;
    updateView();
  } else {
    page = 0;
    const query = el('search').value.trim();
    const pageId = pageQuery(query) || /^(page\d{3}):/i.exec(query)?.[1].toLowerCase();
    const split = gallery.pageSplits[pageId];
    el('empty').textContent = split && split !== 'train'
      ? `Page ${Number(pageId.slice(4))} was held out for ${split === 'test' ? 'testing' : 'validation'}.`
      : 'No sampled subword matches these filters.';
    history.replaceState(null, '', urlFor(null));
  }
  populateList();
}

function filter(requestedId, draw) {
  const source = el('source').value;
  const q = normal(el('search').value);
  const pageId = pageQuery(q);
  matches = gallery.rows.filter(r => (source === 'all' || r.source === source) &&
    (pageId ? r.id.startsWith(pageId + ':') : r.search.includes(q)));
  const preferred = matches.find(r => r.id === requestedId);
  show(preferred || matches[0], preferred ? draw : undefined);
}

try {
  const response = await fetch('./gallery.json');
  if (!response.ok) throw new Error('Could not load the training manifest');
  gallery = await response.json();
  const s = gallery.summary;
  const count = gallery.rows.length;
  el('overview').textContent = `${fmt(count)} distinct subwords · ${fmt(s.draws)} training draws · checkpoint at step ${fmt(s.step)}`;
  for (const option of el('source').options) {
    const n = option.value === 'all' ? count : s.sourceCounts[option.value];
    option.textContent += ` (${fmt(n)})`;
  }
  el('trainingInfo').textContent = `The ${s.canvas[1]} × ${s.canvas[0]} pixel canvases were slightly rotated, scaled and shifted together with their masks. Each numbered view replays that batch item using the saved seed and trainer on ${s.replayDevice.toUpperCase()}. The original run used ${s.trainingDevice.toUpperCase()}. The per-subword draw counts agree with the checkpoint log.`;
  el('run').textContent = s.run;
  el('hash').textContent = s.checkpointSha256;
  for (const r of gallery.rows) r.search = normal([r.id, r.text, ...r.templates].join(' '));
  function restore() {
    const p = new URLSearchParams(location.search);
    el('source').value = Object.hasOwn(sourceName, p.get('source')) ? p.get('source') : 'all';
    const id = decodeURIComponent(location.hash.slice(1));
    el('search').value = p.get('q') || (id && !gallery.rows.some(r => r.id === id) ? id : '');
    filter(id, p.get('draw'));
  }
  restore();
  el('search').addEventListener('input', () => filter(selected?.id));
  el('source').addEventListener('change', () => filter(selected?.id));
  el('view').addEventListener('change', updateView);
  el('back').addEventListener('click', () => show(matches[(page - 1) * pageSize]));
  el('forward').addEventListener('click', () => show(matches[(page + 1) * pageSize]));
  window.addEventListener('hashchange', restore);
} catch (error) {
  el('overview').textContent = error.message;
}
