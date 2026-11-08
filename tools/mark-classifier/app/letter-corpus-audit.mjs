const form = document.getElementById('filters');
const grid = document.getElementById('grid');
const raw = document.getElementById('raw');
const before = document.getElementById('before');
const dialog = document.getElementById('detail');
const message = document.getElementById('message');
const state = new URLSearchParams(location.search);
let offset = Number(state.get('offset') || 0);
let revision = 0;
let total = 0;
for (const element of form.elements) {
  if (element.name && state.has(element.name)) element.value = state.get(element.name);
}
raw.checked = state.get('raw') === '1' || state.get('sort') === 'raw';
before.checked = state.get('before') === '1';

function add(host, tag, text, className) {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (className) node.className = className;
  host.append(node);
  return node;
}

const percent = n => (100 * n).toFixed(1) + '%';
const viewer = row => 'http://127.0.0.1:8770/viewer.html?subword=' + encodeURIComponent(row.id) + '#' + row.page;
const image = row => '/api/image?id=' + encodeURIComponent(row.id) + '&raw=' + Number(raw.checked)
  + '&before=' + Number(before.checked) + '&generation=' + encodeURIComponent(row.referenceRevision || 'original');

function describe(row) {
  const sort = form.elements.sort.value;
  const result = row[sort === 'raw' ? 'raw' : 'completed'];
  let score = percent(result.differenceFraction) + ' ink differs';
  if (sort === 'letter') score = percent(result.worstBodyDifference) + ' worst letter difference';
  if (sort === 'boundary') score = result.boundaryP95PenWidths === null
    ? 'Boundary comparison unavailable' : result.boundaryP95PenWidths.toFixed(1) + ' pen widths boundary shift';
  if (sort === 'count') score = 'Body count difference ' + Math.abs(result.modelBodies - row.expectedBodies);
  if (sort === 'raw') score = percent(result.differenceFraction) + ' raw ink differs';
  if (row.reference === 'partial') score = 'Partial or invalid comparison reference';
  return score + ' · Bodies ' + result.modelBodies + ' / expected ' + row.expectedBodies;
}

function inspect(row) {
  document.getElementById('title').textContent = row.id + ' · ' + row.text;
  document.getElementById('large').src = image(row);
  document.getElementById('facts').textContent = describe(row) + (row.reviewNote ? ' · ' + row.reviewNote : '');
  document.getElementById('pageLink').href = viewer(row);
  document.getElementById('diagnostics').textContent = JSON.stringify(row, null, 2);
  dialog.showModal();
}

function card(row) {
  const box = document.createElement('article');
  box.className = 'card';
  const caption = add(box, 'div', undefined, 'caption');
  caption.title = row.wordText;
  const link = add(caption, 'a', row.id, 'address');
  link.href = viewer(row);
  link.target = '_blank';
  link.rel = 'noopener';
  add(caption, 'bdi', row.text).dir = 'rtl';
  const button = add(box, 'button', undefined, 'preview');
  button.type = 'button';
  button.setAttribute('aria-label', 'Inspect ' + row.id);
  const picture = add(button, 'img');
  picture.src = image(row);
  picture.loading = 'lazy';
  picture.alt = 'Cutter, Mask2Former and differences for ' + row.text;
  button.addEventListener('click', () => inspect(row));
  add(box, 'p', describe(row), 'metrics');
  const use = row.usedForTraining ? 'Crop used in original training' : 'Crop unused in original training';
  const reference = row.reference === 'partial' ? 'Partial / raster issue' : row.reference;
  add(box, 'p', row.split + ' page · ' + use + ' · Reference ' + reference, 'provenance');
  if (row.reviewNote) add(box, 'p', row.reviewNote, 'provenance');
  return box;
}

async function load() {
  const ticket = ++revision;
  grid.setAttribute('aria-busy', 'true');
  message.textContent = 'Loading comparisons…';
  const params = new URLSearchParams(new FormData(form));
  params.set('offset', offset);
  if (raw.checked) params.set('raw', '1');
  if (before.checked) params.set('before', '1');
  history.replaceState(null, '', '?' + params);
  try {
    const response = await fetch('/api/search?' + params);
    if (!response.ok) throw new Error('Could not load the comparison report.');
    const data = await response.json();
    if (ticket !== revision) return;
    total = data.total;
    grid.replaceChildren(...data.rows.map(card));
    document.getElementById('count').textContent = total
      ? (offset + 1).toLocaleString() + '–' + (offset + data.rows.length).toLocaleString() + ' of ' + total.toLocaleString()
      : 'No matching subwords';
    document.getElementById('previous').disabled = offset === 0;
    document.getElementById('next').disabled = offset + data.rows.length >= total;
    message.textContent = '';
  } catch (error) {
    message.textContent = error.message;
  } finally {
    if (ticket === revision) grid.setAttribute('aria-busy', 'false');
  }
}

form.addEventListener('submit', event => { event.preventDefault(); offset = 0; load(); });
for (const element of form.querySelectorAll('select')) element.addEventListener('change', () => {
  if (element.name === 'sort' && element.value === 'raw') raw.checked = true;
  offset = 0;
  load();
});
raw.addEventListener('change', load);
before.addEventListener('change', () => { offset = 0; load(); });
document.getElementById('previous').addEventListener('click', () => {
  offset = Math.max(0, offset - Number(form.elements.limit.value)); load(); window.scrollTo(0, 0);
});
document.getElementById('next').addEventListener('click', () => {
  offset += Number(form.elements.limit.value); load(); window.scrollTo(0, 0);
});
document.getElementById('close').addEventListener('click', () => dialog.close());
fetch('/api/summary').then(response => response.json()).then(data => {
  const seconds = Math.round(data.elapsedSeconds);
  document.getElementById('summary').textContent = data.examples.toLocaleString() + ' predictions · '
    + data.pages + ' pages · Inference ' + seconds.toLocaleString() + ' seconds'
    + (data.expectedSubwords ? ' · Corpus ' + data.expectedSubwords.toLocaleString() + ' subwords' : '')
    + (data.failures ? ' · ' + data.failures + ' source failures' : '')
    + (data.referenceUpdate ? ' · Cutter updated; predictions unchanged' : '');
}).catch(() => { document.getElementById('summary').textContent = 'Whole-corpus comparison'; });
fetch('/api/review-batches').then(response => {
  if (!response.ok) throw new Error('Review batches unavailable');
  return response.json();
}).then(data => {
  const select = document.getElementById('reviewBatch');
  for (const [id, batch] of Object.entries(data.batches)) {
    const option = document.createElement('option');
    option.value = id;
    option.textContent = batch.title + ' (' + batch.ids.length + ')';
    select.append(option);
  }
  if (data.batches[state.get('reviewBatch')]) select.value = state.get('reviewBatch');
  before.disabled = !data.beforeAvailable;
  if (before.disabled) before.checked = false;
}).catch(() => { before.checked = false; }).finally(load);
