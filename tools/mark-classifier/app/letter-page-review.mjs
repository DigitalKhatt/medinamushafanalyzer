const data = window.PAGE_COMPARISON;
const $ = s => document.querySelector(s);
const palette = {count:'#c43f3f', boundary:'#ca791a', unresolved:'#8553ba', minor:'#318c99', same:'#8b99a9'};
const labels = {count:'Region count differs', boundary:'Boundary shifted > 2 px', unresolved:'Incomplete / unassigned', minor:'Small boundary difference', same:'Exact agreement'};
const ranks = {unresolved:4,count:3,boundary:2,minor:1,same:0};
const pct = value => `${(100*value).toFixed(1)}%`;
const key = `letter-page-review:${data.summary.page}:${data.summary.modelSha256}:${data.summary.templateSha256}`;
let notes = {};
try { notes = JSON.parse(localStorage.getItem(key) || '{}'); } catch {}
let selected = data.rows.find(r => r.id === decodeURIComponent(location.hash.slice(1)))?.id || null;
let zoom = 1, filtered = [];
const result = row => row.results[$('#raw').checked ? 'raw' : 'completed'];
const active = () => data.rows.find(row => row.id === selected);
const suffix = () => $('#raw').checked ? '-raw' : '';
const ns = 'http://www.w3.org/2000/svg';
function make(tag, text, className) {const node=document.createElement(tag);if(text!==undefined)node.textContent=text;if(className)node.className=className;return node;}
$('#heading').textContent = `Page ${Number(data.summary.page.slice(4))} · Mask2Former vs template cuts`;
$('#subtitle').textContent = `${data.rows.length} subword bodies · held-out ${data.summary.pageSplit} page · checkpoint ${data.summary.trainingStep} · tracker ${data.summary.templateTrackerEnabled?'enabled':'disabled'}`;
$('#scope-note').textContent = 'Page frame, surah headings, verse markers and basmala openings follow the existing corpus exclusions. Notes stay separate from training annotations.';
const stat = (value, text) => {const node=make('span',undefined,'stat');node.append(make('b',String(value)),document.createTextNode(text));return node;};
function summary(){const counts=$('#raw').checked?data.summary.rawCounts:data.summary.counts;$('#summary').replaceChildren(stat(data.rows.length,'subwords'),stat(counts.count||0,'count differences'),stat(counts.boundary||0,'shifted boundaries'),stat(counts.unresolved||0,'need template / coverage review'));}
const [vx,vy,vw,vh] = data.viewBox;
$('#hit-map').setAttribute('viewBox',`${vx} ${vy} ${vw} ${vh}`);
for (const row of data.rows) {
  const rect=document.createElementNS(ns,'rect');const [x,y,w,h]=row.bbox;
  for(const [name,value] of Object.entries({x:x-2,y:y-2,width:w+4,height:h+4,rx:3,tabindex:0,role:'button','aria-label':`${row.id} ${row.text}`}))rect.setAttribute(name,value);
  rect.dataset.id=row.id;
  const title=document.createElementNS(ns,'title');title.textContent=`${row.id} ${row.text}`;rect.append(title);
  rect.addEventListener('click',()=>select(row.id));
  rect.addEventListener('keydown',event=>{if(event.key==='Enter'||event.key===' '){event.preventDefault();select(row.id);}});
  $('#hit-map').append(rect);
}
function resizeSheet() {
  $('#sheet').style.width=`${vw*zoom}px`;$('#sheet').style.height=`${vh*zoom}px`;
  Object.assign($('#page-image').style,{width:`${data.width*zoom}px`,height:`${data.height*zoom}px`,left:`${-vx*zoom}px`,top:`${-vy*zoom}px`});
  $('#zoom-value').textContent=`${Math.round(zoom*100)}%`;
}
function setZoom(value, anchor) {
  const viewport=$('#viewport'),vr=viewport.getBoundingClientRect(),sr=$('#sheet').getBoundingClientRect();
  const point=anchor||{x:vr.left+vr.width/2,y:vr.top+vr.height/2};
  const x=(point.x-sr.left)/zoom, y=(point.y-sr.top)/zoom;
  zoom=Math.max(.12,Math.min(8,value));resizeSheet();
  const after=$('#sheet').getBoundingClientRect();
  viewport.scrollLeft+=after.left+x*zoom-point.x;viewport.scrollTop+=after.top+y*zoom-point.y;
}
function fit(){const viewport=$('#viewport');zoom=Math.min((viewport.clientWidth-40)/vw,(viewport.clientHeight-28)/vh);resizeSheet();viewport.scrollLeft=0;viewport.scrollTop=0;}
function pageImage() {
  summary();
  const mode=$('#mode').value;const variant=['model','difference','cuts'].includes(mode)?suffix():'';
  $('#page-image').src=`assets/page-${mode}${variant}.png`;
  $('#page-image').alt=`Page ${Number(data.summary.page.slice(4))}, ${$('#mode').selectedOptions[0].textContent}${variant?' (raw model)':''}`;
  const legend=mode==='difference'?[['#414b57','Agreement'],['#e76f20','Instance assignment differs'],['#ce2e6a','Body / kashida role differs'],['#7c41be','Unassigned or uncertain reference']]:mode==='cuts'?[['#0070c8','Template boundary'],['#dc2870','Model boundary'],['#24303d','Same boundary pixel']]:mode==='input'?[['#191919','Subword body ink'],['#9da4ae','Dots / marks (context)']]:[['#326baf','Letter bodies (multiple colors)'],['#edbd48','Kashidas'],['#9b9b9b','Unassigned ink']];
  $('#legend').replaceChildren(...legend.map(([color,label])=>{const node=make('span',undefined,'key'),swatch=make('i',undefined,'swatch');swatch.style.background=color;node.append(swatch,label);return node;}));
  for(const rect of $('#hit-map').children){const row=data.rows.find(r=>r.id===rect.dataset.id),p=result(row).priority;rect.setAttribute('class',`hit ${$('#boxes').checked&&ranks[p]>=2?'flag ':''}${row.id===selected?'selected':''}`);rect.style.setProperty('--flag',palette[p]);}
}
function updateQueue(chooseVisible=false) {
  const filter=$('#filter').value,query=$('#find').value.trim();
  filtered=data.rows.filter(row=>(filter==='all'||filter==='review'&&ranks[result(row).priority]>=2||filter===result(row).priority)&&(!query||`${row.id} ${row.text}`.includes(query)));
  filtered.sort($('#sort').value==='page'?(a,b)=>a.line-b.line||a.word-b.word||a.subword-b.subword:(a,b)=>ranks[result(b).priority]-ranks[result(a).priority]||result(b).changedFraction-result(a).changedFraction||a.line-b.line||a.word-b.word||a.subword-b.subword);
  $('#items').replaceChildren(...filtered.map(row=>{const option=make('option',`${notes[row.id]?.decision&&notes[row.id].decision!=='unreviewed'?'✓ ':''}${row.id.split(':')[1]} · ${row.text}`);option.value=row.id;return option;}));
  const included=filtered.some(r=>r.id===selected);
  if(!selected||chooseVisible&&!included){selected=filtered[0]?.id||null;history.replaceState(null,'',selected?`#${encodeURIComponent(selected)}`:location.pathname);}
  $('#queue-count').textContent=`${filtered.length} of ${data.rows.length}${selected&&!filtered.some(r=>r.id===selected)?' · selected case outside filter':''}`;
  if(included||filtered.some(r=>r.id===selected))$('#items').value=selected;else $('#items').selectedIndex=-1;
  const index=filtered.findIndex(r=>r.id===selected);$('#previous').disabled=index<=0;$('#next').disabled=!filtered.length||index===filtered.length-1;
  if(!filtered.length)$('#items').append(make('option','No matching subwords'));
}
function panel(title, src, full=false) {const box=make('div',undefined,`crop${full?' full':''}`),img=make('img');img.src=src;img.alt=title;box.append(make('h3',title),img);return box;}
function details() {
  const row=active(),detail=$('#detail');detail.replaceChildren();
  if(!row){detail.append(make('p','Select a subword on the page.','empty'));return;}
  const r=result(row),title=make('div',undefined,'case-title');title.append(make('span',row.id,'case-id'),make('span',row.text,'arabic'));detail.append(title);
  const badge=make('span',labels[r.priority],'badge flag');badge.style.setProperty('--flag',palette[r.priority]);detail.append(badge,make('span',row.complete?'Template complete':'Template partial','badge'));
  let explanation=`${pct(r.changedFraction)} of source ink changes instance or role after matching region IDs. `;
  if(!row.referenceStatus.valid)explanation+='The template reference is incomplete or fails raster checks; this case is excluded from aggregate agreement scores. ';
  if(r.rawUnassignedPixels)explanation+=`${r.rawUnassignedPixels} source pixels have no model assignment. `;
  detail.append(make('p',explanation,'description'));
  const grid=make('div',undefined,'comparison');grid.append(panel('Template regions',row.images.template),panel(`Mask2Former${suffix()?' · raw':''}`,row.images['model'+suffix()]),panel('Cut boundaries · blue template / pink model',row.images['cuts'+suffix()],true),panel('Source body',row.images.input),panel('Changed ink',row.images['difference'+suffix()]));detail.append(grid);
  const numbers=make('div',undefined,'numbers');
  for(const [name,value] of [['Bodies · template / model',`${r.referenceBodies} / ${r.modelBodies}`],['Kashidas · template / model',`${r.referenceKashidas} / ${r.modelKashidas}`],['Boundary pixels > 2 px apart',r.farBoundaryPixels],['Unknown reference / model pixels',r.unknownPixels],['Mean region overlap',r.metrics?pct(r.metrics.meanIoUPerReference):'Not scored']])numbers.append(make('span',name),make('b',String(value)));
  detail.append(numbers,make('div',`Templates: ${row.templates.join(', ')||'Single shape / no template cut'}`,'templates'));
  $('#decision').value=notes[row.id]?.decision||'unreviewed';$('#note').value=notes[row.id]?.note||'';
}
function select(id,pan=false) {
  selected=id;history.replaceState(null,'',`#${encodeURIComponent(id)}`);updateQueue();details();pageImage();
  if(pan){const row=active(),[x,y,w,h]=row.bbox,viewport=$('#viewport');viewport.scrollLeft=(x-vx+w/2)*zoom-viewport.clientWidth/2;viewport.scrollTop=(y-vy+h/2)*zoom-viewport.clientHeight/2;}
}
function saveNote(){const row=active();if(!row)return;notes[row.id]={decision:$('#decision').value,note:$('#note').value,updatedAt:new Date().toISOString()};try{localStorage.setItem(key,JSON.stringify(notes));$('#save-state').textContent='Saved in this browser. Training labels are unchanged.';}catch{$('#save-state').textContent='Browser storage unavailable. Export notes to keep them.';}updateQueue();}
$('#mode').addEventListener('change',pageImage);$('#boxes').addEventListener('change',pageImage);$('#raw').addEventListener('change',()=>{updateQueue();details();pageImage();});
for(const id of ['filter','find','sort'])$('#'+id).addEventListener('input',()=>{updateQueue(true);details();pageImage();});
$('#items').addEventListener('change',()=>select($('#items').value,true));
$('#previous').onclick=()=>{const i=filtered.findIndex(r=>r.id===selected);if(i>0)select(filtered[i-1].id,true);};
$('#next').onclick=()=>{const i=filtered.findIndex(r=>r.id===selected);if(i+1<filtered.length)select(filtered[i+1].id,true);};
$('#in').onclick=()=>setZoom(zoom*1.35);$('#out').onclick=()=>setZoom(zoom/1.35);$('#fit').onclick=fit;$('#actual').onclick=()=>setZoom(1);
$('#viewport').addEventListener('wheel',event=>{if(event.ctrlKey||event.metaKey){event.preventDefault();setZoom(zoom*Math.exp(-event.deltaY*.003),{x:event.clientX,y:event.clientY});}},{passive:false});
$('#decision').addEventListener('change',saveNote);$('#note').addEventListener('input',saveNote);
$('#export').onclick=()=>{const payload={schemaVersion:1,page:data.summary.page,modelSha256:data.summary.modelSha256,templateSha256:data.summary.templateSha256,sourceSha256:data.summary.sourceSha256,purpose:'Visual review notes, not approved training masks',notes};const blob=new Blob([JSON.stringify(payload,null,2)],{type:'application/json'}),url=URL.createObjectURL(blob),link=make('a');link.href=url;link.download=`${data.summary.page}-segmentation-review-notes.json`;link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);};
$('#copy').onclick=async()=>{const row=active();if(!row)return;try{await navigator.clipboard.writeText(JSON.stringify({id:row.id,text:row.text,templateComplete:row.complete,letters:row.letters,templates:row.templates,comparison:result(row),review:notes[row.id]||null},null,2));$('#copy').textContent='Copied';setTimeout(()=>$('#copy').textContent='Copy selected case',1500);}catch{$('#save-state').textContent='Copy is unavailable here; use Export review notes.';}};
updateQueue();details();pageImage();requestAnimationFrame(fit);
