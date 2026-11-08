const el = id => document.getElementById(id);
const format = number => number.toLocaleString('en-US');
const sourceNames = {'machine-weak': 'Template cuts', 'human-reviewed': 'Human-reviewed'};
const changeNames = {changed: 'Changed mask', new: 'New subword', same: 'Same mask'};
const norm = value => value.normalize('NFD').replace(/[\p{M}ـ]/gu, '').toLowerCase().trim();
const numericPage = value => /^(?:page\s*)?(\d{1,3})$/i.exec(value);
const cardImage = (row, view = 'base') => './' + view + '/' + row.file + '.png?v=' + gallery.datasetSha256.slice(0, 12);
let gallery, selected, matches = [], page = 0, revision = -1, frozen = false, token;
let visible = 48;
let excluded = new Set();
const saving = new Set();

async function request(url, options) {
  const response = await fetch(url, options);
  if (!response.ok) {
    let message = 'Request failed (' + response.status + ').';
    try { message = (await response.json()).error || message; } catch {}
    throw new Error(message);
  }
  return response.json();
}

function acceptState(state) {
  if (state.datasetSha256 !== gallery.datasetSha256) throw new Error('Dataset changed. Reload the grid.');
  if (state.revision < revision) return;
  revision = state.revision;
  excluded = new Set(state.excluded);
  frozen = Boolean(state.frozenOutput);
  if (state.token) token = state.token;
}

function message(value) {
  el('notice').textContent = value;
  el('detailNotice').textContent = value;
}

const filterKeys = {q: 'search', selection: 'selection', source: 'source', change: 'change', size: 'size'};
function updateURL() {
  const url = new URL(location.href);
  for (const [key, id] of Object.entries(filterKeys)) {
    const value = el(id).value.trim();
    if (!value || (id === 'size' ? value === '48' : value === 'all')) url.searchParams.delete(key);
    else url.searchParams.set(key, value);
  }
  if (page > 0) url.searchParams.set('page', page + 1);
  else url.searchParams.delete('page');
  url.hash = selected?.id || '';
  history.replaceState(null, '', url);
}

function element(tag, cls, label) {
  const result = document.createElement(tag);
  if (cls) result.className = cls;
  if (label) result.textContent = label;
  return result;
}

function toggleStyle(button, row, detail) {
  const isExcluded = excluded.has(row.id);
  button.textContent = isExcluded ? 'Restore' : detail ? 'Exclude from training' : 'Exclude';
  button.className = 'toggle' + (isExcluded ? ' restore' : '');
  button.setAttribute('aria-label', (isExcluded ? 'Restore ' : 'Exclude ') + row.id);
  button.disabled = frozen || saving.has(row.id);
}

function card(row) {
  const isExcluded = excluded.has(row.id);
  const item = element('article', 'card' + (isExcluded ? ' excluded' : ''));
  item.dataset.id = row.id;
  const preview = element('button', 'preview');
  preview.setAttribute('aria-label', 'Inspect ' + row.id);
  const img = element('img'); img.src = cardImage(row); img.alt = ''; img.loading = 'lazy';
  const caption = element('span', 'caption');
  const word = element('bdi', 'arabic', row.text); word.dir = 'rtl'; word.lang = 'ar';
  const flag = isExcluded ? 'Excluded' : row.source === 'human-reviewed' ? 'Human-reviewed' :
    row.comparison === 'changed' ? changeNames.changed : '';
  caption.append(word, element('span', 'flag', flag));
  preview.append(img, caption); preview.onclick = () => inspect(row);
  const footer = element('div', 'cardFooter'), button = element('button');
  toggleStyle(button, row, false); button.onclick = () => toggle(row);
  footer.append(element('code', '', row.id), button);
  item.append(preview, footer);
  return item;
}

function render() {
  const q = norm(el('search').value), pageQuery = numericPage(q);
  const prefix = pageQuery ? 'page' + pageQuery[1].padStart(3, '0') + ':' : null;
  const sel = el('selection').value, src = el('source').value, change = el('change').value;
  matches = gallery.rows.filter(r =>
    (sel === 'all' || excluded.has(r.id) === (sel === 'excluded')) &&
    (src === 'all' || r.source === src) && (change === 'all' || r.comparison === change) &&
    (prefix ? r.id.startsWith(prefix) : r.search.includes(q)));
  const continuous = el('size').value === 'all';
  const size = continuous ? Math.max(1, matches.length) : Number(el('size').value);
  page = Math.max(0, Math.min(page, Math.ceil(matches.length / size) - 1));
  const start = page * size, end = Math.min(continuous ? visible : start + size, matches.length);
  el('grid').replaceChildren(...matches.slice(start, end).map(card));
  el('empty').hidden = Boolean(matches.length);
  el('range').textContent = matches.length ? format(start + 1) + '–' + format(end) + ' of ' + format(matches.length) : '0 results';
  for (const b of document.querySelectorAll('.prevPage')) b.disabled = page === 0;
  for (const b of document.querySelectorAll('.nextPage')) b.disabled = end >= matches.length;
  for (const b of document.querySelectorAll('.prevPage,.nextPage')) b.hidden = continuous;
  el('more').hidden = !continuous || end >= matches.length;
  el('includedCount').textContent = format(gallery.rows.length - excluded.size);
  el('excludedCount').textContent = format(excluded.size);
  if (frozen) message('This selection has been frozen for training.');
  if (selected) renderDetail();
  updateURL();
}

function showMore() {
  if (el('size').value !== 'all' || el('more').hidden) return;
  const count = el('grid').childElementCount;
  visible = Math.min(matches.length, count + 48);
  el('grid').append(...matches.slice(count, visible).map(card));
  el('range').textContent = '1–' + format(visible) + ' of ' + format(matches.length);
  el('more').hidden = visible >= matches.length;
}

function renderDetail() {
  const row = selected;
  el('detailTitle').textContent = row.text;
  el('detailId').textContent = row.id;
  const view = el('view').value, img = el('detailImage');
  img.src = cardImage(row, view === 'native' ? 'native' : 'base');
  img.classList.toggle('ink', view === 'ink');
  img.alt = (view === 'ink' ? 'Original ink' : 'Instance masks') + ' for ' + row.id;
  const details = [sourceNames[row.source], row.bodies + (row.bodies === 1 ? ' body' : ' bodies'),
    row.kashidas + (row.kashidas === 1 ? ' kashida' : ' kashidas')];
  if (row.comparison) details.push(changeNames[row.comparison]);
  if (excluded.has(row.id)) details.push('Excluded');
  el('detailMeta').textContent = details.join(' · ');
  el('detailTemplates').textContent = row.templates.join(' · ');
  toggleStyle(el('detailToggle'), row, true);
  const i = matches.indexOf(row);
  el('detailPrevious').disabled = i < 1;
  el('detailNext').disabled = i < 0 || i === matches.length - 1;
}

function inspect(row) {
  if (!row) return;
  selected = row;
  const i = matches.indexOf(row);
  if (i >= 0 && el('size').value !== 'all') page = Math.floor(i / Number(el('size').value));
  render();
  if (!el('detail').open) el('detail').showModal();
}

async function toggle(row) {
  if (!row || saving.has(row.id) || frozen) return;
  const desired = !excluded.has(row.id);
  saving.add(row.id); message(''); render();
  try {
    const state = await request('./api/selection', {method: 'POST',
      headers: {'Content-Type': 'application/json', 'X-Review-Token': token},
      body: JSON.stringify({id: row.id, excluded: desired, datasetSha256: gallery.datasetSha256})});
    acceptState(state);
    message((desired ? 'Excluded ' : 'Restored ') + row.id + '. Saved.');
  } catch (error) {
    try { acceptState(await request('./api/selection')); } catch {}
    message('Could not confirm the change. ' + error.message);
  } finally {
    saving.delete(row.id); render();
    if (!el('detail').open) el('grid').querySelector('[data-id="' + row.id + '"] .toggle')?.focus({preventScroll: true});
  }
}

function navigate(direction) {
  inspect(matches[matches.indexOf(selected) + direction]);
}

try {
  const [index, state] = await Promise.all([request('./gallery.json'), request('./api/selection')]);
  gallery = index;
  acceptState(state);
  const humans = gallery.rows.filter(r => r.source === 'human-reviewed').length;
  el('summary').textContent = 'Mask2Former · ' + format(gallery.rows.length) + ' multi-letter training subwords · ' +
    format(gallery.rows.length - humans) + ' template cuts · ' + humans + ' human-reviewed';
  el('compareControl').hidden = !gallery.previousRun;
  for (const option of el('change').options) {
    if (option.value !== 'all') option.textContent += ' (' + format(gallery.rows.filter(r => r.comparison === option.value).length) + ')';
  }
  for (const r of gallery.rows) r.search = norm([r.id, r.text, ...r.templates].join(' '));
  const url = new URL(location.href), initial = decodeURIComponent(url.hash.slice(1));
  for (const [key, id] of Object.entries(filterKeys)) {
    const value = url.searchParams.get(key), field = el(id);
    if (value && (!field.options || [...field.options].some(o => o.value === value))) field.value = value;
    field.addEventListener(id === 'search' ? 'input' : 'change', () => { page = 0; visible = 48; render(); });
  }
  page = Math.max(0, parseInt(url.searchParams.get('page'), 10) - 1 || 0);
  render();
  if (initial) inspect(gallery.rows.find(r => r.id === initial));
  el('more').onclick = showMore;
  new IntersectionObserver(entries => {
    if (entries.some(entry => entry.isIntersecting)) showMore();
  }, {rootMargin: '350px'}).observe(el('more'));
  for (const cls of ['prevPage', 'nextPage']) for (const b of document.querySelectorAll('.' + cls)) {
    b.onclick = () => { page += cls === 'prevPage' ? -1 : 1; render(); el('grid').scrollIntoView({block: 'start'}); };
  }
  el('close').onclick = () => el('detail').close();
  el('detail').addEventListener('close', () => {
    const id = selected?.id; selected = null; updateURL();
    if (id) el('grid').querySelector('[data-id="' + id + '"] .preview')?.focus({preventScroll: true});
  });
  el('detailPrevious').onclick = () => navigate(-1);
  el('detailNext').onclick = () => navigate(1);
  el('detailToggle').onclick = () => toggle(selected);
  el('view').onchange = renderDetail;
  window.addEventListener('focus', async () => {
    try { const fresh = await request('./api/selection'); acceptState(fresh); render(); }
    catch (error) { message(error.message); }
  });
} catch (error) {
  message(error.message); el('summary').textContent = 'Could not open the training set.';
}
