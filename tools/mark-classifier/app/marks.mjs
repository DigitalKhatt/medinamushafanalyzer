const get = id => document.getElementById(id);
const fields = ['q', 'page', 'base', 'expected', 'predicted', 'verdict'];
const labels = {match:'Exact match', different:'Classifier disagrees', 'accepted-form':'Accepted form',
  missing:'No separate mask', ambiguous:'Ambiguous character', unmatched:'Unmatched ink'};
const methods = {'native-geometry':'Native shape match', 'contour-quota':'Unique contour count',
  'remaining-slot':'Only compatible character left'};
const limit = 36;
let rows = [], offset = 0, request = 0, selected = '', timer;

function element(tag, text, className) {
  const node = document.createElement(tag);
  if (text != null) node.textContent = text;
  if (className) node.className = className;
  return node;
}
async function api(action, params) {
  const response = await fetch('/api/automatic/marks/'+action+'?'+params);
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || response.statusText);
  return data;
}
function filterParams() {
  const p = new URLSearchParams();
  for (const key of fields) if (get(key).value) p.set(key,get(key).value);
  if (offset) p.set('offset',offset);
  return p;
}
function updateURL() {
  const p = filterParams();
  if (selected) p.set('id',selected);
  history.replaceState(null,'',location.pathname+(p.size?'?'+p:''));
}
function mask(row, context=false) {
  const img = element('img');
  img.src = '/api/automatic/marks/image.svg?'+new URLSearchParams({id:row.id,context:context?'1':'0'});
  img.alt = row.verdict==='missing' ? 'Source word containing the expected character' :
    context ? 'Selected ink highlighted in its source word' : (row.expected_type || 'Assigned ink');
  if (!context) img.loading = 'lazy';
  return img;
}
function glyph(c) { return c && /\p{Mark}/u.test(c) ? '◌'+c : c; }
function card(row) {
  const button = element('button',null,'card');
  button.type = 'button'; button.dataset.id = row.id;
  button.append(row.verdict==='missing'?element('span','No separate mask','noink'):mask(row),
    element('strong',row.expected_type || row.classifier_type || 'Unresolved ink'),
    element('small',row.subword_id,'muted'),
    element('small',labels[row.verdict],['match','accepted-form'].includes(row.verdict)?'muted':'difference'));
  button.onclick = () => { select(row); updateURL(); };
  return button;
}
function select(row) {
  selected = row?.id || '';
  for (const button of get('grid').children) {
    const active = button.dataset.id===selected;
    button.classList.toggle('active',active);
    button.setAttribute('aria-pressed',String(active));
  }
  const panel = get('detail');
  panel.replaceChildren(element('h2', row ? row.expected_type || 'Assigned ink' : 'Mark details'));
  if (!row) { panel.append(element('p','Select a mask to see its ink in the source word.')); return; }
  const word = element('bdi',row.text,'arabic');word.dir='rtl';
  panel.append(word,element('p',row.subword_id,'muted'),mask(row,true));
  const list = element('dl');
  function fact(name, val) { list.append(element('dt',name),element('dd',val)); }
  if (row.character_id) {
    fact('Transcript character',glyph(row.source_character)+' · '+row.codepoint);
    fact('Word offset',row.word_offset+' (zero-based)');
  } else if (row.candidates.length) fact('Possible offsets',row.candidates.join(', '));
  fact('Owner',row.base_character ? row.base_character+' · offset '+row.base_offset : 'Word-level sign');
  if (row.equivalence) fact('Transcript equivalence',row.equivalence);
  if (row.classifier_type) fact('Classifier',row.classifier_type+' · '+(100*row.score).toFixed(1)+'%');
  if (row.raw_type && row.raw_type!==row.classifier_type) fact('Raw model output',row.classifier_kind+' / '+row.raw_type);
  fact('Validation',labels[row.verdict]);
  if (row.method) fact('Assignment',methods[row.method] || row.method);
  if (row.path_ids.length) fact('Source paths',row.path_ids.join(' + '));
  const report = element('a','Open character in the page report');
  report.href = '/api/automatic/marks/report/'+encodeURIComponent(row.page_id)+'.html#'+encodeURIComponent(row.character_id || row.subword_id);
  panel.append(list,report);
}
async function search(initialId='') {
  const ticket = ++request;
  get('error').hidden = true;
  try {
    const p = filterParams(); p.set('limit',limit);
    const result = await api('search',p);
    if (ticket!==request) return;
    if (offset && offset>=result.total) { offset=0; return search(initialId); }
    rows = result.rows;
    get('count').textContent = result.total ? (offset+1).toLocaleString()+'–'+(offset+rows.length).toLocaleString()+
      ' of '+result.total.toLocaleString() : 'No results';
    get('prev').disabled = offset===0; get('next').disabled = offset+limit>=result.total;
    get('grid').replaceChildren(...rows.map(card));
    select(rows.find(r=>r.id===initialId) || rows[0]);
    // Keep the URL for filters short. Record a selected mask only after a user explicitly opens it.
    if (initialId && selected===initialId) updateURL();
    else { const p=filterParams(); history.replaceState(null,'',location.pathname+(p.size?'?'+p:'')); }
  } catch (error) { if (ticket===request) showError(error); }
}
function showError(error) { get('error').textContent=error.message; get('error').hidden=false; }
get('filters').oninput = () => {
  clearTimeout(timer); offset=0; timer=setTimeout(()=>search(),180);
};
get('filters').onsubmit = e => { e.preventDefault(); clearTimeout(timer); offset=0; search(); };
get('filters').onreset = () => { clearTimeout(timer); offset=0; setTimeout(()=>search(),0); };
get('prev').onclick = () => { offset=Math.max(0,offset-limit); search(); };
get('next').onclick = () => { offset+=limit; search(); };
try {
  const meta=await api('metadata','');
  get('coverage').textContent=meta.masks.toLocaleString()+' assigned masks across '+meta.pages+' pages · '+
    (meta.counts.different || 0)+' classifier disagreements · '+(meta.counts.missing || 0)+' transcript characters without separate ink';
  for (const [field,values] of [['expected',meta.expected],['predicted',meta.predicted],['base',meta.bases]]) {
    for (const val of values) get(field).add(new Option(val,val));
  }
  const p=new URLSearchParams(location.search);
  for (const key of fields) if (p.has(key)) get(key).value=p.get(key);
  const n=Number(p.get('offset')); offset=Number.isSafeInteger(n) && n>=0 && n<=2000000 ? n : 0;
  await search(p.get('id') || '');
} catch (error) { showError(error); }
