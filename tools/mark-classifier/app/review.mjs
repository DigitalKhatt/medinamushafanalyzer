import {makeInkMask, connectedComponents, proposeLines} from './core.mjs';
import {scopePath} from './scope.mjs';
const $=id=>document.getElementById(id);
const colours={'subword-ink':'#317bc5',dot:'#a15aba',mark:'#168c79',symbol:'#bc526b',unknown:'#d78c2f'};
let corpus,page,doc,proposals,source,selectedId,visible=[],busy=false,cropBounds,hitRegions=[];
const canvas=$('crop'),ctx=canvas.getContext('2d');
async function api(url,options){const response=await fetch(url,options);const value=await response.json();if(!response.ok)throw new Error(value.error||`Request failed (${response.status})`);return value;}
function notice(message){$('notice').textContent=message;}
async function task(fn){if(busy)return;busy=true;document.body.classList.add('busy');try{await fn();}catch(error){notice(error.message);}finally{busy=false;document.body.classList.remove('busy');}}
const jsonHeaders=()=>({'Content-Type':'application/json','X-Annotation-Token':corpus.token});
function state(item){
  const record=$('queue').value==='subwords'?proposals.subwordReview[item.id]:item.review;
  return record?.decision||'pending';
}
function current(){return visible.find(item=>item.id===selectedId);}
function isSubword(){return $('queue').value==='subwords';}
function candidateParts(candidate){const ids=new Set(candidate.regionIds);return doc.parts.filter(p=>ids.has(p.id));}
function candidateBounds(candidate){
  const parts=candidateParts(candidate);
  let boxes=parts.map(p=>p.bbox);
  if(!boxes.length)boxes=candidate.pathIds.map(id=>proposals.paths.find(p=>p.id===id)?.bbox).filter(Boolean);
  return union(boxes);
}
function union(boxes){if(!boxes.length)return [0,0,100,100];const x=Math.min(...boxes.map(b=>b[0])),y=Math.min(...boxes.map(b=>b[1]));return[x,y,Math.max(...boxes.map(b=>b[0]+b[2]))-x,Math.max(...boxes.map(b=>b[1]+b[3]))-y];}
function refreshList(keep=true){
  if(!proposals)return;
  const prior=selectedId;
  const sourceItems=isSubword()?proposals.subwords:proposals.candidates;
  const query=$('search').value.trim().toLowerCase(),status=$('status').value,kind=$('kind').value;
  visible=sourceItems.filter(item=>{
    if(item.inScope===false)return false;
    if($('queue').value==='marks'&&item.kind==='subword-ink')return false;
    if(status!=='all'&&state(item)!==status)return false;
    if(!isSubword()&&kind!=='all'&&(item.review?.role||item.kind)!==kind)return false;
    if($('flagged').checked&&!item.issues.length)return false;
    return !query||[item.id,item.text,item.markType,item.kind,item.wordText].join(' ').toLowerCase().includes(query);
  });
  $('items').replaceChildren();
  const fragment=document.createDocumentFragment();
  for(const item of visible){
    const button=document.createElement('button');button.className='item';button.dataset.id=item.id;
    const top=document.createElement('span');top.className='item-top';
    const subtype=item.review?.markType||item.markType;
    const title=document.createElement('span');title.textContent=isSubword()?item.text:subtype==='unknown'?item.kind==='subword-ink'?'Subword body':'Unresolved ink':subtype;
    if(isSubword()){title.dir='rtl';title.lang='ar';title.className='arabic-label';}
    const flag=document.createElement('span');flag.className='flag';flag.textContent=state(item)==='approved'?'✓':item.issues.length?'!':'';
    top.append(title,flag);const small=document.createElement('small');small.textContent=`Line ${item.line||'—'} · ${item.id}`;
    button.append(top,small);button.onclick=()=>select(item.id);fragment.append(button);
  }
  $('items').append(fragment);
  $('queue-summary').textContent=`${visible.length.toLocaleString()} in this queue · ${proposals.candidates.filter(c=>c.review?.decision==='approved').length} ink labels approved`;
  $('kind').disabled=isSubword();
  select(keep&&visible.some(i=>i.id===prior)?prior:visible[0]?.id);
}
function select(id){
  selectedId=id;
  for(const button of $('items').children)button.classList.toggle('active',button.dataset.id===id);
  const item=current();$('empty').hidden=!!item;canvas.hidden=!item;
  $('approve').disabled=$('reject').disabled=!item;
  if(!item){$('selection-title').textContent='Queue complete';$('word').textContent='';$('context-text').textContent='Choose another page, queue or filter.';$('review-state').textContent='No selection';$('reopen').hidden=true;$('mark-fields').hidden=$('subword-fields').hidden=true;$('warnings').replaceChildren();$('match-info').textContent='';$('edit').hidden=true;return;}
  $('edit').hidden=false;
  const sub=isSubword()?item:proposals.subwords.find(s=>s.id===item.subwordId);
  const word=proposals.words.find(w=>w.id===(isSubword()?item.wordId:sub?.wordId||item.wordId));
  $('word').textContent=word?.text||'—';
  $('breadcrumb').textContent=`${page.id.toUpperCase()} / LINE ${item.line||'—'} / ${word?.id||'SOURCE INK'}`;
  $('context-text').textContent=sub?`Subword: ${sub.text} · original SVG ink · ${isSubword()?'validate body boundary and text':'validate this dot / mark instance'}`:'Ink not resolved by the vector analyzer';
  $('selection-title').textContent=isSubword()?'Subword body & text':item.kind==='subword-ink'?'Uncut subword body':'Dot / mark label';
  $('review-state').textContent=state(item)==='pending'?'Draft proposal':state(item)==='approved'?'Approved · saved to workbench':state(item)==='changed'?'Existing review · check against this proposal set':'Rejected · ink remains unlabelled';
  $('mark-fields').hidden=isSubword();$('subword-fields').hidden=!isSubword();
  $('reopen').hidden=state(item)==='pending';
  $('reject').disabled=state(item)==='approved';
  $('warnings').replaceChildren(...item.issues.map(message=>{const li=document.createElement('li');li.textContent=message;return li;}));
  $('body-options').replaceChildren();
  if(isSubword()){
    const saved=proposals.subwordReview[item.id];
    $('subword-text').value=saved?.text||item.text;
    const bodyIds=saved?.reviewed?new Set(proposals.candidates.filter(c=>c.subwordId===item.id&&candidateParts(c).some(p=>p.subwordBody)).map(c=>c.id)):new Set(item.bodyCandidateIds);
    for(const cid of item.candidateIds){
      const c=proposals.candidates.find(c=>c.id===cid),label=document.createElement('label'),input=document.createElement('input');
      input.type='checkbox';input.value=cid;input.checked=bodyIds.has(cid);input.disabled=!c.canApply;
      input.onchange=draw;
      const text=document.createElement('span');text.textContent=`${c.kind==='subword-ink'?'Body piece':c.markType==='unknown'?'Unresolved ink':c.markType} · ${c.regionIds.length} regions${!c.canApply?' · geometry unresolved':''}`;
      label.append(input,text);$('body-options').append(label);
    }
    $('approve').textContent='Approve body & text';$('approve').disabled=state(item)==='approved';
    $('match-info').textContent='Subword supervision is separate from character and ink-role supervision.';
  }else{
    const role=item.review?.role||item.kind;
    $('role').value=['dot','mark','symbol','other'].includes(role)?role:'mark';
    $('attach-label').hidden=!sub;
    $('mark-type').value=item.review?.markType||item.markType||'unknown';
    $('approve').textContent='Approve label  A';
    $('approve').disabled=!item.canApply||item.kind==='subword-ink'||item.protected||state(item)==='approved';
    $('match-info').textContent=`${item.regionIds.length} source regions · ${(item.match.sourceCoverage*100).toFixed(1)}% source overlap${item.protected?' · existing labels protected':''}`;
  }
  const candidates=isSubword()?item.candidateIds.map(id=>proposals.candidates.find(c=>c.id===id)):[item];
  const ids=[...new Set(candidates.flatMap(c=>c.regionIds))];
  const workbenchParams = new URLSearchParams({regions: ids.join(','), proposalSource: proposals.proposalSource});
  if (sub) workbenchParams.set('subword', sub.id);
  $('edit').href = `/index.html?${workbenchParams}#${page.id}`;
  $('position').textContent=`${visible.indexOf(item)+1} / ${visible.length}`;
  notice('');$('zoom').value='100';
  setBounds(word,item);draw();
}
function setBounds(word,item){
  const all=word?proposals.candidates.filter(c=>c.wordId===word.id):[];
  const boxes=(all.length?all:isSubword()?item.candidateIds.map(id=>proposals.candidates.find(c=>c.id===id)):[item]).map(candidateBounds);
  const [x,y,w,h]=union(boxes),pad=38;
  const left=Math.max(0,Math.floor(x-pad)),top=Math.max(0,Math.floor(y-pad));
  cropBounds=[left,top,Math.min(doc.raster.width-left,Math.ceil(w+pad*2)),Math.min(doc.raster.height-top,Math.ceil(h+pad*2))];
}
function paintRuns(context,runs,colour){
  context.fillStyle=colour;
  const width=doc.raster.width,[cx,cy,cw,ch]=cropBounds;
  for(let j=0;j<runs.length;j+=2){let start=runs[j],remaining=runs[j+1];while(remaining){const y=Math.floor(start/width),x=start%width,n=Math.min(remaining,width-x);if(y>=cy&&y<cy+ch&&x<cx+cw&&x+n>cx)context.fillRect(x-cx,y-cy,n,1);start+=n;remaining-=n;}}
}
function draw(){
  const item=current();if(!item||!source||!cropBounds)return;
  const [x,y,w,h]=cropBounds;canvas.width=w;canvas.height=h;
  ctx.fillStyle='#fff';ctx.fillRect(0,0,w,h);ctx.save();
  if(proposals.sourceScope){ctx.translate(-x,-y);ctx.clip(scopePath(proposals.sourceScope,doc.raster),'evenodd');ctx.translate(x,y);}
  ctx.drawImage(source,x,y,w,h,0,0,w,h);
  const wanted=isSubword()?new Set([...$('body-options').querySelectorAll('input:checked')].map(i=>i.value)):new Set([item.id]);
  hitRegions=[];
  if($('overlay').checked){
    ctx.globalAlpha=.65;
    for(const candidate of proposals.candidates){
      if(candidate.inScope===false)continue;
      const bounds=candidateBounds(candidate);
      if(bounds[0]>x+w||bounds[0]+bounds[2]<x||bounds[1]>y+h||bounds[1]+bounds[3]<y)continue;
      const active=wanted.has(candidate.id);
      const colour=active?(isSubword()?colours['subword-ink']:'#eb7727'):colours[candidate.kind]||colours.unknown;
      for(const part of candidateParts(candidate)){paintRuns(ctx,part.runs,colour);hitRegions.push({candidate,part});}
    }
    ctx.globalAlpha=1;
  }else{
    for(const c of proposals.candidates)if(c.inScope!==false)for(const part of candidateParts(c))hitRegions.push({candidate:c,part});
  }
  // Bounding boxes make tiny selected marks discoverable without changing their masks.
  ctx.lineWidth=1;ctx.strokeStyle='#245d52';ctx.setLineDash([3,3]);
  for(const cid of wanted){const c=proposals.candidates.find(c=>c.id===cid);if(c){const b=candidateBounds(c);ctx.strokeRect(b[0]-x-4,b[1]-y-4,b[2]+8,b[3]+8);}}
  ctx.setLineDash([]);
  ctx.restore();
  const base=Math.max(1,Math.min(($('viewer').clientWidth-16)/w,($('viewer').clientHeight-16)/h));
  const scale=base*Number($('zoom').value)/100;
  canvas.style.width=`${w*scale}px`;canvas.style.height=`${h*scale}px`;
}
canvas.onclick=event=>{
  const rect=canvas.getBoundingClientRect();const x=Math.floor((event.clientX-rect.left)*canvas.width/rect.width+cropBounds[0]),y=Math.floor((event.clientY-rect.top)*canvas.height/rect.height+cropBounds[1]);const pixel=y*doc.raster.width+x;
  const found=hitRegions.find(({part})=>part.runs.some((s,i)=>i%2===0&&pixel>=s&&pixel<s+part.runs[i+1]));
  if(found&&!isSubword()){
    if(!visible.some(i=>i.id===found.candidate.id)){ $('queue').value='all';$('status').value='all';$('kind').value='all';$('search').value='';refreshList(false); }
    select(found.candidate.id);$('items').querySelector('.active')?.scrollIntoView({block:'nearest'});
  }
};
async function reloadData(){
  const data=await api(`/api/pages/${page.id}`);doc=data.annotation;
  proposals=await api(`/api/pages/${page.id}/proposals?source=${encodeURIComponent($('proposal-source').value)}`);
  $('proposal-source').value=proposals.proposalSource;
  const url=new URL(location.href);url.searchParams.set('proposalSource',proposals.proposalSource);history.replaceState(null,'',url);
  const label=proposals.proposalSource==='automatic'?'Latest automatic proposals':'Original proposal snapshot';
  $('proposal-origin').textContent=proposals.proposalSource==='automatic'?'Reload reads the current automatic proposals. Your approvals are saved separately.':'Older proposals, with their original review history.';
  $('alignment').textContent=`${label} · ${(proposals.alignment.matchedFraction*100).toFixed(1)}% source overlap · revision ${doc.revision}${proposals.sourceScope?` · Quran text area only; ${proposals.alignment.excludedByFrame} excluded regions hidden`:` · ${proposals.alignment.unresolvedRegions} unresolved regions retained`}`;
}
async function openPage(id){
  const data=await api(`/api/pages/${id}`);page=data.page;doc=data.annotation;
  const available=corpus.pages.find(p=>p.id===id);
  $('proposal-source').querySelector('[value="automatic"]').disabled=!available.hasAutomaticProposals;
  $('proposal-source').querySelector('[value="legacy"]').disabled=available.hasLegacyProposals===false;
  if($('proposal-source').selectedOptions[0]?.disabled)$('proposal-source').value=available.hasAutomaticProposals?'automatic':'legacy';
  const raster=doc?.raster||{width:Math.round(page.viewBox[2]*corpus.config.raster_scale),height:Math.round(page.viewBox[3]*corpus.config.raster_scale),scale:corpus.config.raster_scale,threshold:corpus.config.ink_threshold,chroma:corpus.config.max_chroma,viewBox:page.viewBox};
  const img=new Image();img.src=`/api/pages/${id}/source.svg`;await img.decode();
  source=document.createElement('canvas');source.width=raster.width;source.height=raster.height;
  const context=source.getContext('2d',{willReadFrequently:true});context.fillStyle='#fff';context.fillRect(0,0,raster.width,raster.height);context.drawImage(img,0,0,raster.width,raster.height);
  if(!doc){
    notice('Preparing source ink. All labels begin unreviewed.');
    const mask=makeInkMask(context.getImageData(0,0,raster.width,raster.height).data,raster.threshold,raster.chroma);
    const {parts}=connectedComponents(mask,raster.width,raster.height);
    doc={schemaVersion:1,characterMetadataVersion:1,pageId:id,sourceSha256:page.sha256,revision:0,raster,parts,inkPixels:parts.reduce((n,p)=>n+p.area,0),lines:proposeLines(mask,raster.width,raster.height),groups:{},nextGroup:1};
    await api(`/api/pages/${id}/annotation`,{method:'PUT',headers:jsonHeaders(),body:JSON.stringify(doc)});
  }
  // Existing pages are never saved or migrated just by navigating the review UI.
  await reloadData();location.hash=id;$('page').value=id;$('workbench').href=`/index.html#${id}`;refreshList(false);
  if(proposals.sourceScope){
    const clean=new Image();clean.src=`/api/pages/${id}/source.svg?scope=interior`;await clean.decode();
    context.fillStyle='#fff';context.fillRect(0,0,raster.width,raster.height);context.drawImage(clean,0,0,raster.width,raster.height);draw();
  }
}
async function review(action){
  const item=current();if(!item)return;
  const index=visible.indexOf(item),nextId=visible[index+1]?.id;
  const bodyCandidateIds=[...$('body-options').querySelectorAll('input:checked')].map(i=>i.value);
  await api(`/api/pages/${page.id}/proposal-review`,{method:'PUT',headers:jsonHeaders(),body:JSON.stringify({
    action:isSubword()?`${action}-subword`:action,id:item.id,revision:doc.revision,proposalSha256:proposals.proposalSha256,proposalSource:proposals.proposalSource,
    role:$('role').value,markType:$('mark-type').value.trim()||'unknown',text:$('subword-text').value,bodyCandidateIds,
    attachSubword:$('attach-subword').checked,
  })});
  await reloadData();refreshList(true);
  if($('status').value==='pending'&&nextId&&visible.some(i=>i.id===nextId))select(nextId);
  $('items').querySelector('.active')?.scrollIntoView({block:'nearest'});
  notice(action==='approve'?'Approved and saved to the workbench.':action==='reject'?'Proposal rejected. Source ink retained.':'Review reopened.');
}
$('approve').onclick=()=>task(()=>review('approve'));
$('reject').onclick=()=>task(()=>review('reject'));
$('reopen').onclick=()=>task(()=>review('reopen'));
$('page').onchange=()=>task(()=>openPage($('page').value));
$('reload').onclick=()=>task(async()=>{await openPage(page?.id||$('page').value);notice('Proposals reloaded. Existing labels and reviews are preserved.');});
$('proposal-source').onchange=()=>task(async()=>{
  const url=new URL(location.href);for(const key of ['candidate','suggestRole','suggestType'])url.searchParams.delete(key);history.replaceState(null,'',url);
  $('search').value='';await reloadData();refreshList(false);
});
for(const id of ['queue','status','kind','flagged'])$(id).onchange=()=>refreshList(false);
$('search').oninput=()=>refreshList(false);
$('zoom').oninput=$('overlay').onchange=draw;
function navigate(delta){const i=visible.findIndex(item=>item.id===selectedId);if(visible[i+delta]){select(visible[i+delta].id);$('items').querySelector('.active')?.scrollIntoView({block:'nearest'});}}
$('previous').onclick=()=>navigate(-1);$('next').onclick=()=>navigate(1);
$('export').onclick=()=>task(async()=>{const result=await api(`/api/pages/${page.id}/export`);notice(`Exported reviewed labels to ${result.directory}`);});
window.addEventListener('resize',draw);
window.addEventListener('hashchange',()=>{
  const id=location.hash.slice(1);
  if(!busy&&id!==page?.id&&corpus?.pages.some(p=>p.id===id&&p.hasProposals))task(()=>openPage(id));
});
window.addEventListener('keydown',event=>{if(busy||['INPUT','SELECT','TEXTAREA'].includes(event.target.tagName))return;const key=event.key.toLowerCase();if(key==='a'&&!$('approve').disabled){event.preventDefault();$('approve').click();}else if(key==='r'&&!$('reject').disabled){event.preventDefault();$('reject').click();}else if(key==='n'||key==='arrowright')navigate(1);else if(key==='p'||key==='arrowleft')navigate(-1);});
await task(async()=>{
  corpus=await api('/api/corpus');const pages=corpus.pages.filter(p=>p.hasProposals);
  if(!pages.length)throw new Error('No vector proposals yet. Run prepare_vector_review.py, then reload.');
  $('page').replaceChildren(...pages.map(p=>new Option(`${p.id} · ${p.split}`,p.id)));
  const requestedSource=new URLSearchParams(location.search).get('proposalSource');
  if(['automatic','legacy'].includes(requestedSource))$('proposal-source').value=requestedSource;
  const initial=location.hash.slice(1);await openPage(pages.some(p=>p.id===initial)?initial:pages.find(p=>p.id==='page003')?.id||pages[0].id);
  const query=new URLSearchParams(location.search);
  if(query.get('candidate')){
    $('queue').value=query.get('queue')==='subwords'?'subwords':'all';$('status').value='all';
    $('search').value=query.get('candidate');refreshList(false);
    const role=query.get('suggestRole'),type=query.get('suggestType');
    if(['dot','mark','symbol','other'].includes(role)&&current()&&state(current())!=='approved'){
      $('role').value=role;if(type&&/^[a-z][a-z0-9_-]{0,63}$/.test(type))$('mark-type').value=type;
      notice('Model suggestion prefilled. Check the ink before approving; no review was saved automatically.');
    }
  }
});
