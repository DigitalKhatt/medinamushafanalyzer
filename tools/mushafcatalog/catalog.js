import { $, api, el, fmt, inkImage, kv, labels } from './ui.js';
import { openWord } from './inspector.js';
import { ResultGrid } from './result-grid.js';

const form = $('filters'), names = {letters:'letters',marks:'marks',subwords:'subwords',words:'words',parts:'letter-body parts'};
const group = $('forms'), formButtons = [...group.querySelectorAll('button')];
const defaults = {match:'class',sort:'location',metric:'width',limit:'400'};
const openingQuery = {kind:'letters',metric:'kashidaLeft',sort:'largest',context:'word',kleft:'1'};
const limitOf = p => /^(max|[1-9]\d{0,5})$/.test(p.get('limit')||'') ? p.get('limit') : defaults.limit;
const pageSize = p => limitOf(p)==='max' ? Infinity : Number(limitOf(p));
const locationState = () => new URLSearchParams(location.search || openingQuery);
const kindOf = p => Object.hasOwn(names,p.get('kind')) ? p.get('kind') : 'letters';
function viewOf(p, kind) {
  const context = p.get('context');
  return {pair:p.has('pair'),kleft:p.has('kleft'),kright:p.has('kright'),marks:p.get('marks')!=='0',
    context:context==='word'?'word':kind==='letters' && context!=='subword'?'match':'crop'};
}
let facets, query = null, selected = null, controlsKey = null;
const BATCH = 100;
const CACHE_PAGES = 24;

function navigate(p, replace=false) {
  const target = p.size ? '/?' + p : '/';
  if (location.pathname+location.search!==target) history[replace?'replaceState':'pushState'](null,'',target);
  render();
}
function setOptions(select, values, first, value='') {
  select.replaceChildren(new Option(first,''));
  for (const s of values) select.add(new Option(s,s));
  select.value = value;
}
function restore(p, kind) {
  $('kind').value = kind;
  for (const node of form.querySelectorAll('[data-kinds]')) {
    node.hidden = !node.dataset.kinds.split(' ').includes(kind);
    for (const c of node.matches('input,select,button') ? [node] : node.querySelectorAll('input,select,button')) c.disabled = node.hidden;
  }
  for (const c of form.querySelectorAll('[name]')) c.value = p.get(c.name) || defaults[c.name] || '';
  const letters = kind==='letters';
  $('char').classList.toggle('wide',!letters);
  $('char').dir = letters?'rtl':'auto';
  $('char').placeholder = letters?'بَت':kind==='marks'?'mark or address':'Arabic or address';
  $('char').title = letters?'One or more consecutive letters inside one subword; optional marks follow their letter.':'Search text or a native address.';
  $('char').value = letters?(p.get('sequence') || p.get('q') || ''):(p.get('q') || '');
  if (letters && p.has('sequence') && p.has('q') && !p.has('within')) form.elements.within.value=p.get('q');
  if (letters && !p.has('match') && (p.has('sequence') || (p.has('q') && !/^page\d/.test(p.get('q')))))
    $('match').value='letter'; // existing exact-search bookmarks
  if ($('match').value==='') $('match').value=p.get('match')==='exact'?'letter':'class';
  setOptions($('cls'),kind==='marks'?facets.expected:facets.classes,kind==='marks'?'any mark class':'any class',
    p.get(kind==='marks'?'expected':'class') || '');
  if (kind==='marks') form.elements.ownerClass.value = p.get('class') || '';
  const forms = (p.get('forms') || p.get('form') || '').split(',');
  formButtons.forEach(b=>b.setAttribute('aria-pressed',String(forms.includes(b.dataset.form))));
  $('unknownForm').checked=forms.includes('unknown');
  form.elements.coverage.querySelector('[value="partial"]').disabled = kind!=='words';
  const metric=$('metric');
  metric.querySelector('optgroup').disabled = !letters;
  // Keep old measurement bookmarks readable without offering empty metrics.
  metric.querySelector('option[data-legacy]')?.remove();
  const legacy=p.get('metric');
  if (letters && ['advance','spine'].includes(legacy)) {
    const option=new Option(legacy,legacy);
    option.hidden=true;option.dataset.legacy='';metric.add(option);metric.value=legacy;
  }
  const [largest,smallest]=metric.value==='width'?['widest first','narrowest first']:
    metric.value==='height'?['tallest first','shortest first']:
    metric.value.startsWith('kashida')?['longest first','shortest first']:['largest first','smallest first'];
  $('sort').querySelector('[value="largest"]').textContent=largest;
  $('sort').querySelector('[value="smallest"]').textContent=smallest;
  $('sort').querySelector('[value="confidence"]').disabled = !letters && kind!=='marks';
  $('context').checked = p.get('context')==='word';
  $('subword').checked = letters && p.get('context')==='subword';
  $('showMarks').checked = p.get('marks')!=='0';
  for (const k of ['pair','kleft','kright']) $(k).checked=p.has(k);
  const limit = limitOf(p);
  if (![...$('limit').options].some(o=>o.value===limit)) $('limit').add(new Option(limit,limit));
  $('limit').value=limit;
}
function collect() {
  const p=new URLSearchParams({kind:$('kind').value}), kind=kindOf(p);
  const main=$('char').value.trim();
  if (main) p.set(kind==='letters' && !/^page\d/.test(main)?'sequence':'q',main);
  for(const c of form.querySelectorAll('[name]')) {
    if (c.disabled || !c.value || c.name==='ownerClass') continue;
    if (['metric','sort'].includes(c.name) && c.value===defaults[c.name]) continue;
    p.set(c.name,c.name==='page'?c.value.trim().replace(/[–—]/g,'-'):c.value.trim());
  }
  if (kind==='marks' && form.elements.ownerClass.value) p.set('class',form.elements.ownerClass.value);
  if (!$('cls').disabled && $('cls').value) p.set(kind==='marks'?'expected':'class',$('cls').value);
  const forms=formButtons.filter(b=>!b.disabled && b.getAttribute('aria-pressed')==='true').map(b=>b.dataset.form);
  if (!$('unknownForm').disabled && $('unknownForm').checked) forms.push('unknown');
  if (forms.length) p.set('forms',forms.join(','));
  if ($('limit').value!==defaults.limit) p.set('limit',$('limit').value);
  if ($('context').checked) p.set('context','word');
  else if (!$('subword').disabled && $('subword').checked) p.set('context','subword');
  if (!$('showMarks').checked) p.set('marks','0');
  for (const k of ['pair','kleft','kright']) if (!$(k).disabled && $(k).checked) p.set(k,'1');
  return p;
}
form.onsubmit = event => {
  event.preventDefault();
  // The native importer can publish measurements while this tab is open.
  clearQuery();
  navigate(collect());
};
$('kind').onchange=()=>{
  const old=locationState(), p=new URLSearchParams({kind:$('kind').value});
  for(const k of ['page','limit','marks']) if(old.has(k)) p.set(k,old.get(k));
  if (old.get('context')==='word') p.set('context','word');
  if (/^page\d/.test(old.get('q')||'')) p.set('q',old.get('q'));
  navigate(p);
};
for (const b of formButtons) b.onclick=()=>{
  b.setAttribute('aria-pressed',String(b.getAttribute('aria-pressed')!=='true'));
  form.requestSubmit();
};
$('more').onclick=()=>{
  const open=$('advanced').hidden; $('advanced').hidden=!open; $('more').setAttribute('aria-expanded',String(open));
};
$('reset').onclick=()=>navigate(new URLSearchParams({kind:$('kind').value}));
$('cls').onchange=()=>{if ($('kind').value==='marks') form.elements.expected.value=$('cls').value};
form.elements.expected.onchange=()=>{if ($('kind').value==='marks') $('cls').value=form.elements.expected.value};
for (const k of ['limit','sort','match']) $(k).onchange=()=>form.requestSubmit();
$('metric').onchange=()=>{
  if ($('kind').value==='letters' && !$('kleft').checked && !$('kright').checked) {
    const m=$('metric').value;
    $('kleft').checked=['kashida','kashidaLeft'].includes(m); $('kright').checked=['kashida','kashidaRight'].includes(m);
  }
  form.requestSubmit();
};
for (const [id,context] of [['context','word'],['subword','subword']]) $(id).onchange=()=>{
  const p=locationState(); if (!p.has('kind')) p.set('kind',$('kind').value);
  if ($(id).checked) p.set('context',context); else p.delete('context');
  navigate(p);
};
for (const k of ['pair','kleft','kright']) $(k).onchange=()=>{
  const p=locationState(); if (!p.has('kind')) p.set('kind',$('kind').value);
  if ($(k).checked) p.set(k,'1'); else p.delete(k);
  navigate(p);
};
$('showMarks').onchange=()=>{
  const p=locationState();
  if ($('showMarks').checked) p.delete('marks'); else p.set('marks','0');
  navigate(p);
};
function move(sign) {
  const p=locationState(), step=pageSize(p);
  if (!Number.isFinite(step)) return;
  p.set('offset',String(Math.max(0,Number(p.get('offset')||0)+sign*step)));
  p.delete('word');p.delete('focus');navigate(p);
}
$('prev').onclick=()=>move(-1); $('next').onclick=()=>move(1);

function card(row, kind, view) {
  const id=row.sequence_ids||row.id, b=el('button',null,'card'); b.type='button';
  b.dataset.focus=id; b.dataset.word=row.word_id;
  const art=inkImage(kind,id,view.context,'art',view), t=el('div',null,'card-title');
  const arabic=el('span',row.text,'arabic');arabic.lang='ar';
  t.append(arabic,el('span',row.group_form||row.form||''));
  const info=el('div',null,'card-info');info.append(t);
  if (kind==='letters' && view.pair && row.pair_next)
    info.append(el('div',row.text+'ـ'+row.pair_next,'arabic pair-text'));
  const caption = kind==='marks'
    ? [row.expected_type || 'Unlinked', row.verdict==='different'?'→ '+row.classifier_type:'',labels[row.verdict]]
    : kind==='parts'?[row.role,row.variant]:kind==='subwords'?[row.letter_count?row.letter_count+' letters':row.category,labels[row.coverage]]
    :[labels[row.coverage]];
  info.append(el('div',caption.filter(Boolean).join(' · '),'caption'+(['different','missing'].includes(row.verdict)?' issue':'')));
  if (row.metric_value!=null) info.append(el('div',$('metric').selectedOptions[0]?.textContent+' '+Number(row.metric_value.toFixed(2))+' px','caption'));
  info.append(el('div',row.id,'address'));b.append(art,info);
  b.title=[row.text,row.id,caption.filter(Boolean).join(' · ')].join('\n');
  return b;
}
$('cards').onclick=e=>{
  const retry=e.target.closest('[data-retry]');
  if(retry){loadPage(query,Number(retry.dataset.retry));return;}
  const b=e.target.closest('.card');if(!b)return;
  const p=locationState();p.set('word',b.dataset.word);p.set('focus',b.dataset.focus);navigate(p);
  if (matchMedia('(max-width:760px)').matches) $('selected').scrollIntoView({block:'start'});
};
function showSelection(p) {
  const id=p.get('focus');
  if(selected && selected.dataset.focus!==id) selected.classList.remove('sel');
  selected=id?$('cards').querySelector('[data-focus="'+CSS.escape(id)+'"]'):null;
  selected?.classList.add('sel');
  const row=id && (query?.selectedRow && (query.selectedRow.sequence_ids||query.selectedRow.id)===id
    ? query.selectedRow : [...(query?.pages.values()||[])].flat().find(r=>(r.sequence_ids||r.id)===id));
  if (query) query.selectedRow=row;
  const facts=$('selectedFacts');
  facts.replaceChildren();
  if (row) {
    if (row.group_form) facts.append(kv('form of match',row.group_form));
    facts.append(kv('letter bodies',labels[row.coverage]),kv('ink width × height',row.width==null?'—':row.width+' × '+row.height+' px'));
    if (row.confidence!=null || row.score!=null) facts.append(kv('confidence',((row.confidence??row.score)*100).toFixed(1)+'%'));
    if (row.quality) facts.append(kv('cut quality',row.quality));
    if (row.verdict) facts.append(kv('validation',labels[row.verdict]));
  }
  document.querySelector('.app').classList.toggle('has-selection',p.has('word'));
  const kind = kindOf(p);
  openWord(p.get('word'),id,kind,viewOf(p,kind));
}
function distribution(data,total) {
  const box=$('statsView'), dist=data.distribution, n=total-data.unmeasured;
  box.replaceChildren(kv('matches',fmt(total)),kv('measured',fmt(n)),kv('unmeasured',fmt(data.unmeasured)));
  if (!n) { box.append(el('p','No measurements for this metric.','hint')); return; }
  function median(pairs) {
    let idx=0, lo, hi;
    for(const [v,num] of pairs) {
      idx+=num;
      if (lo===undefined && idx>Math.floor((n-1)/2)) lo=v;
      if (idx>Math.floor(n/2)) { hi=v;break; }
    }
    return (lo+hi)/2;
  }
  const med=median(dist), mad=median(dist.map(([v,c])=>[Math.abs(v-med),c]).sort((a,b)=>a[0]-b[0]));
  const min=dist[0][0], max=dist.at(-1)[0], px=n=>Number(n.toFixed(2))+' px';
  box.append(kv('median',px(med)),kv('MAD',px(mad)),kv('min / max',px(min)+' / '+px(max)));
  const nBins=max===min?1:24, step=(max-min)/nBins||1, bins=Array(nBins).fill(0);
  for (const [v,c] of dist) bins[Math.min(nBins-1,Math.floor((v-min)/step))]+=c;
  const hist=el('div',null,'histogram'), peak=Math.max(...bins);
  bins.forEach((c,i)=>{
    const bar=el('i');bar.style.height=(100*c/peak)+'%';
    bar.title=px(min+i*step)+'–'+px(Math.min(max,min+(i+1)*step))+' · '+fmt(c);hist.append(bar);
  });
  box.append(hist);
}
function paging(q) {
  const end=Math.min(q.total,q.offset+q.size);
  $('prev').disabled=q.offset===0||!Number.isFinite(q.size);$('next').disabled=end>=q.total;
  $('summary').textContent=!q.total?'0 matches':q.offset===0 && end===q.total?'All '+fmt(q.total)+' matches':
    q.offset>=end?'0 of '+fmt(q.total)+' matches':fmt(q.offset+1)+'–'+fmt(end)+' of '+fmt(q.total)+' matches';
}
function clearQuery() {
  if (!query) return;
  query.controller.abort(); clearTimeout(query.timer); query.grid.destroy(); query=null;
}
function rowAt(q,index) {
  const start=Math.floor(index/BATCH)*BATCH;
  return q.errors.get(start) || q.pages.get(start)?.[index-start];
}
function resultCard(q,row,index) {
  if (row && !row.error) {
    const p=locationState();
    const kind=kindOf(p), node=card(row,kind,viewOf(p,kind));
    node.dataset.resultIndex=q.offset+index;
    return node;
  }
  const node=el('button',null,'card placeholder'); node.type='button';
  node.append(el('span',row?'Could not load this result. Click to retry.':'Loading result '+fmt(q.offset+index+1)+'…','hint'));
  if (row) {node.dataset.retry=Math.floor(index/BATCH)*BATCH;node.title=row.error;}
  else {node.disabled=true;node.setAttribute('aria-busy','true');}
  return node;
}
function visibleRange(q,start,end) {
  q.visible=[start,end];
  clearTimeout(q.timer);
  // Dragging the scrollbar across thousands of rows should only request the
  // destination. No requests are needed for the rows passed along the way.
  q.timer=setTimeout(()=>{
    for (let offset=Math.floor(start/BATCH)*BATCH; offset<end; offset+=BATCH)
      if (!q.errors.has(offset)) loadPage(q,offset);
  },80);
}
async function loadPage(q,start) {
  if (query!==q || q.pending.has(start) || q.pages.has(start)) return;
  q.pending.add(start);q.errors.delete(start);
  const p=new URLSearchParams(q.params);p.set('limit',Math.min(BATCH,q.size-start));p.set('offset',q.offset+start);
  try {
    const data=await api('/api/search',p,q.controller.signal);
    if(query!==q)return;
    q.total=data.total; q.pages.set(start,data.rows);
    for (const offset of q.pages.keys()) {
      if(q.pages.size<=CACHE_PAGES) break;
      if(offset+BATCH<=q.visible[0] || offset>=q.visible[1]) q.pages.delete(offset);
    }
    const count=Math.max(0,Math.min(q.size,q.total-q.offset));
    $('welcome').hidden=!!count;
    if(!count) $('welcome').textContent=data.emptyMessage || (q.total?'No results at this offset. Choose Previous results.':'No matches for these filters.');
    q.grid.setCount(count);
    paging(q);
  } catch(err) {
    if (query!==q)return;
    q.errors.set(start,{error:err.message});q.grid.refresh();
    if (!q.pages.size) {
      $('summary').textContent='Search failed';
      $('welcome').hidden=false;$('welcome').textContent=err.message;
      q.failed=true;
    }
  } finally {
    q.pending.delete(start);
    if (query===q) $('cards').removeAttribute('aria-busy');
  }
}
function render() {
  if (!facets)return;
  const p=locationState(),kind=kindOf(p), params=new URLSearchParams(p);
  params.set('kind',kind);
  for (const k of ['word','focus','context','pair','kleft','kright','marks','limit','offset','stats']) params.delete(k);
  const size=pageSize(p), offset=Number.isFinite(size) && /^\d{1,7}$/.test(p.get('offset')||'')?Number(p.get('offset')):0;
  const key=params.toString()+'|'+offset+'|'+size, viewKey=JSON.stringify(viewOf(p,kind));
  const visible=new URLSearchParams(p);
  visible.delete('word');visible.delete('focus');
  if (controlsKey!==visible.toString()) {
    restore(p,kind); controlsKey=visible.toString();
  }
  if(query && query.key===key && !query.failed) {
    if(query.viewKey!==viewKey){query.viewKey=viewKey;$('cards').classList.toggle('with-pair',p.has('pair'));query.grid.refresh(true);}
    else showSelection(p);
    return;
  }
  clearQuery();
  const q=query={key,viewKey,params,offset,size,total:0,pages:new Map(),errors:new Map(),pending:new Set(),
    visible:[0,0],controller:new AbortController()};
  $('cards').replaceChildren();$('cards').setAttribute('aria-busy','true');$('stage').scrollTop=0;
  $('cards').classList.toggle('with-pair',p.has('pair'));
  q.grid=new ResultGrid($('cards'),$('stage'),{
    item:index=>rowAt(q,index),card:(row,index)=>resultCard(q,row,index),
    range:(start,end)=>visibleRange(q,start,end),painted:()=>showSelection(locationState()),
  });
  if (matchMedia('(max-width:760px)').matches) window.scrollTo(0,0);
  $('summary').textContent='Searching…';$('statsView').replaceChildren(el('p','Measuring…','hint'));
  $('welcome').hidden=$('prev').disabled=$('next').disabled=true;
  showSelection(p);
  loadPage(q,0);
  api('/api/statistics',params,q.controller.signal).then(data=>{
    if(query===q) distribution(data,data.unmeasured+data.distribution.reduce((sum,r)=>sum+r[1],0));
  }).catch(err=>{if(query===q) $('statsView').replaceChildren(el('p',err.message,'hint'));});
}
window.addEventListener('popstate',render);
async function start() {
  try {
    facets=await api('/api/facets');
    const n=facets.inventory;
    $('overview').replaceChildren(...[[n.pages,'pages'],[n.words,'words'],[n.subwords,'letter-bearing subwords'],
      [n.letters,'letters'],[n.masks,'assigned ink masks'],[n.segmented,'subwords with body splits']].map(([v,k])=>kv(k,fmt(v))));
    for (const [key,values] of [['expected',facets.expected],['predicted',facets.predicted],
         ['quality',facets.qualities],['ownerClass',facets.classes]]) {
      for (const value of values) form.elements[key].add(new Option(value,value));
    }
    render();
  } catch(err) {
    $('summary').textContent='Catalog unavailable';
    $('welcome').hidden=false;$('welcome').textContent=err.message;
  }
}
start();
