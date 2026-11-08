import {BATCH_SIZE,pageRange,gridWindow,chunkWindow} from './corpus-grid.mjs';
const $=id=>document.getElementById(id);
const kinds={'subword-ink':'Subword body',dot:'Letter dot',mark:'Vowel / mark',symbol:'Quranic sign',unknown:'Unresolved ink',other:'Other ink',body:'Letter body',kashida:'Connection / kashida'};
const colours={'subword-ink':'#317bc5',body:'#317bc5',kashida:'#de8329',dot:'#9958b7',mark:'#168c79',symbol:'#b25470',unknown:'#b99761',other:'#78877f'};
const reasons={'legacy-omission':'No usable proposal from the legacy cache covers this ink.','ambiguous-geometry':'More than one proposal competes for this source region.','incomplete-geometry':'A proposal covers only part of this source region.','proposal-mismatch':'The proposed ink and source region do not match closely enough.','inventory-unassigned':'The transcript inventory did not yield a supported type assignment.','human-draft':'This ink is an unreviewed human draft.'};
let metadata,results,selection,offset=0,sequence=0,detailSequence=0,selectedId=null,similarMode=false;
let searchController,searchParams,resultsSequence=0,visibleCount=0,chunks=new Map(),pendingChunks=new Map(),failedChunks=new Set(),dataVersion=0,gridSignature='',gridFrame,wantedChunks=new Set();
const paramIds={kind:'kind',type:'type',q:'q',source:'source',quality:'quality',reason:'reason',pageSize:'page-size',pageMin:'page-min',pageMax:'page-max',split:'split',metric:'metric',order:'order',min:'min',max:'max'};
const fmt=value=>Number(value||0).toLocaleString();
const pct=value=>value==null?'—':`${(value*100).toFixed(1)}%`;
async function api(path,signal){const response=await fetch(path,{signal});const result=await response.json();if(!response.ok)throw new Error(result.error||`Request failed (${response.status})`);return result;}
function message(text){$('message').textContent=text;}
function element(tag,text,className){const el=document.createElement(tag);if(text!=null)el.textContent=text;if(className)el.className=className;return el;}
function svgElement(tag,attrs){const el=document.createElementNS('http://www.w3.org/2000/svg',tag);for(const [key,value]of Object.entries(attrs))el.setAttribute(key,String(value));return el;}
function getParams(){const params=new URLSearchParams();for(const [key,id]of Object.entries(paramIds))if($(id).value!=='')params.set(key,$(id).value);params.set('limit',BATCH_SIZE);params.set('offset',offset);return params;}
function updateSourceNote(){
  const type=$('type').value;
  $('source').querySelector('[value=final]').textContent=metadata.inference?'Model + reviewed corrections':'Automatic final labels';
  $('quality').querySelector('[value=supported]').textContent=metadata.inference&&$('source').value!=='teacher'?($('source').value==='final'?'Reviewed or model score ≥ 0.90':'Model score ≥ 0.90 · no flags'):'Supported proposals';
  document.querySelector('.weak-note').textContent=metadata.inference?'MODEL OUTPUT · HUMAN CORRECTIONS TAKE PRIORITY':'AUTOMATIC LABELS · HUMAN REVIEWS KEPT SEPARATE';
  const check=$('source').value==='model'&&!metadata.inference?' Triple-dot predictions must pass a visual component/layout check; raw guesses remain available in details.':'';
  const inferenceNote=metadata.inference?' Final labels use model predictions plus human-reviewed overrides. Structural checks '+(metadata.inference.structuralPolicy==='diagnostic-only'?'flag results without replacing the prediction.':'withhold incompatible predictions.') : '';
  if(type==='all'){$('source-note').textContent='Type counts follow the selected label source. Source masks are shared.'+check+inferenceNote;return;}
  const groups=metadata.typesBySource||{final:metadata.types},count=source=>(groups[source]||[]).filter(t=>t.type===type).reduce((n,t)=>n+t.count,0);
  const counts=`Corpus-wide ${type}: proposals ${fmt(count('teacher'))} · model ${fmt(count('model'))} · final ${fmt(count('final'))}.`;
  const support=metadata.learning?.typeSupport?.[type];
  const hamzaType=metadata.inference?.hamzaClassifier&&['hamza','hamza-above','hamza-below'].includes(type);
  const trainingNote=hamzaType?' Predicted by the dedicated Hamza classifier.':support?` Trained on ${fmt(support.trainingUnique)} distinct examples (${fmt(support.humanTraining)} human-reviewed); ${fmt(support.trainingDrawsPerEpoch)} training presentations per epoch.`:metadata.learning&&!metadata.learning.types.includes(type)?' This type has no learned model output yet.':'';
  $('source-note').textContent=counts+((metadata.typeDifferences?.[type]||0)===0?' All three sources agree on the same regions.':' Some labels or region assignments differ between sources.')+trainingNote+check+inferenceNote;
}
function updateTypes(){
  const prior=$('type').value,kind=$('kind').value;
  const groups=metadata.typesBySource||{final:metadata.types},counts=new Map(),names=new Set();
  for(const [source,types]of Object.entries(groups))for(const t of types){
    if((kind==='all'||t.kind===(kind==='unmapped-bodies'?'subword-ink':kind))&&t.type!=='unknown'){
      names.add(t.type);if(source===$('source').value)counts.set(t.type,(counts.get(t.type)||0)+t.count);
    }
  }
  const types=[...names].sort((a,b)=>(counts.get(b)||0)-(counts.get(a)||0)||a.localeCompare(b));
  $('type').replaceChildren(new Option('All types','all'),...types.map(type=>new Option(`${type} · ${fmt(counts.get(type)||0)}`,type)));
  if(names.has(prior))$('type').value=prior;
  const sub=kind==='subwords';$('type').disabled=$('source').disabled=$('reason').disabled=sub;
  for(const option of $('quality').options)option.disabled=sub&&['recovered','unmapped'].includes(option.value);
  if(sub&&['recovered','unmapped'].includes($('quality').value))$('quality').value='all';
  if(kind==='unknown'&&$('quality').value==='supported')$('quality').value='all';
  for(const option of $('metric').options)option.disabled=sub&&!['width','height'].includes(option.value);
  if(sub&&!['width','height'].includes($('metric').value))$('metric').value='width';
  updateSourceNote();
}
async function search(reset=true){
  searchController?.abort();searchController=new AbortController();
  const ticket=++sequence;if(reset)offset=0;similarMode=false;document.body.classList.add('loading');message('Searching the manuscript…');
  try{
    const params=getParams(),data=await api('/api/automatic/search?'+params,searchController.signal);
    if(ticket!==sequence)return;
    results=data;resultsSequence=ticket;searchParams=params;
    const share=new URLSearchParams(params);share.delete('limit');share.delete('offset');history.replaceState(null,'','/corpus.html?'+share);
    message('');drawResults();
  }catch(error){if(ticket===sequence&&error.name!=='AbortError')message(error.message);}
  finally{if(ticket===sequence)document.body.classList.remove('loading');}
}
function drawResults(){
  $('result-count').textContent=`${fmt(results.total)} ${$('kind').value==='subwords'?'subwords':'occurrences'}`;
  $('result-description').textContent=similarMode?'LEARNED VISUAL NEIGHBOURS':`${$('source').selectedOptions[0].textContent.toUpperCase()} · SOURCE MASKS`;
  const range=pageRange(results.total,offset,similarMode?'all':$('page-size').value);
  visibleCount=range.count;chunks=new Map([[0,results.items]]);pendingChunks=new Map();failedChunks=new Set();wantedChunks=new Set();dataVersion++;gridSignature='';
  $('cards').replaceChildren(element('div',null,'cards-window'));
  for(const id of ['pagination','pagination-top'])$(id).textContent=`${fmt(range.start)}–${fmt(range.end)} of ${fmt(results.total)}`;
  for(const id of ['previous','previous-top'])$(id).disabled=!range.previous||similarMode;
  for(const id of ['next','next-top'])$(id).disabled=!range.next||similarMode;
  document.querySelector('.results').scrollTop=0;
  renderGrid();drawHistogram();updateSourceNote();
  if(!results.items.length)message('No matching ink. Try another type, Arabic text or evidence filter.');
}
function createCard(row){
    const card=element('button',null,'card');card.dataset.id=row.id;card.classList.toggle('selected',row.id===selectedId);
    const specimen=element('div',null,'specimen'),img=document.createElement('img');img.src=row.thumbnail;img.alt=`${row.display_type||row.final_type} on ${row.page_id}`;img.loading='lazy';
    if($('same-scale').checked){img.style.width=`${Math.min(180,row.w*.8+8)}px`;img.style.height=`${Math.min(105,row.h*.8+8)}px`;}
    specimen.append(img);
    const info=element('div',null,'card-meta'),top=element('div',null,'card-top');
    const type=row.display_type||row.final_type;
    const title=row.entity==='subword'?row.text:type&&type!=='unknown'?type:kinds[row.display_kind||row.final_kind];
    top.append(element('span',title,row.entity==='subword'?'arabic':'type-name'));
    const dot=element('i',null,'status-dot'),modelPolicy=$('source').value!=='teacher'&&metadata.inference;
    const flagged=modelPolicy?(row.model_confidence<.90||row.prediction_guard||row.final_kind==='unknown'):
      !row.resolution_method&&(row.model_agrees===0||row.final_kind==='unknown'||row.quality<.5);
    dot.classList.toggle('flagged',Boolean(flagged));top.append(dot);
    const bottom=element('div',null,'card-bottom');bottom.append(element('span',`${row.w} × ${row.h} px`),element('span',row.similarity!=null?`${(row.similarity*100).toFixed(1)} similarity`:row.resolution_method==='horizontal-rule'?'Geometry rule passed':row.model_confidence!=null?`${pct(row.model_confidence)} model score`:''));
    info.append(top,bottom,element('span',`${row.page_id} ${row.line?`· line ${row.line}`:''}${row.refined?' · refined':''}`,'card-foot'));
    if(row.assignment_status==='text-unassigned')info.append(element('span','Body recognized · text unassigned','card-foot'));
    else if(row.resolution_method==='horizontal-rule')info.append(element('span','Detached-rule geometry proposal','card-foot'));
    if(row.prediction_guard)info.append(element('span',`Raw guess: ${row.raw_prediction_type} · ${metadata.inference?.structuralPolicy==='diagnostic-only'?'flagged':'withheld'}`,'card-foot'));
    else if(row.model_agrees===0)info.append(element('span',`Model: ${row.prediction_type&&row.prediction_type!=='unknown'?row.prediction_type:kinds[row.prediction_kind]}`,'card-foot'));
    card.append(specimen,info);card.onclick=()=>openDetail(row.id,row.entity);return card;
}
function renderGrid(){
  if(!results||resultsSequence!==sequence)return;
  const host=$('cards'),panel=document.querySelector('.results'),rect=host.getBoundingClientRect(),desktop=getComputedStyle(panel).overflowY==='auto';
  const viewportHeight=desktop?panel.clientHeight:window.innerHeight,viewportTop=desktop?panel.getBoundingClientRect().top:0;
  const size=gridWindow({count:visibleCount,width:host.clientWidth,viewportHeight,scrollOffset:Math.max(0,viewportTop-rect.top),minWidth:parseFloat(getComputedStyle(host).getPropertyValue('--card-min'))||145});
  host.style.height=`${size.height}px`;
  const inner=host.firstElementChild;if(!inner)return;
  inner.style.top=`${size.top}px`;inner.style.gridTemplateColumns=`repeat(${size.columns},minmax(0,1fr))`;
  const signature=[size.start,size.end,size.columns,dataVersion,$('same-scale').checked].join(':');
  if(signature!==gridSignature){
    gridSignature=signature;
    const existing=new Map([...inner.children].map(node=>[Number(node.dataset.index),node]));
    const sameScale=String($('same-scale').checked);let position=0;
    for(let i=size.start;i<size.end;i++){
      const row=chunks.get(Math.floor(i/BATCH_SIZE))?.[i%BATCH_SIZE];let node=existing.get(i);
      if(!node||node.dataset.id!==(row?.id)||node.dataset.sameScale!==sameScale){
        node=row?createCard(row):element('div','Loading ink…','card card-placeholder');
        node.dataset.index=i;node.dataset.sameScale=sameScale;
      }
      // Keep overlapping cards mounted so their images are not cancelled and
      // fetched again every time scrolling advances by one row.
      if(inner.children[position]!==node)inner.insertBefore(node,inner.children[position]||null);
      position++;
    }
    while(inner.children.length>position)inner.lastElementChild.remove();
  }
  const plan=chunkWindow({start:size.start,end:size.end,count:visibleCount});
  wantedChunks=new Set([...plan.visible,...plan.prefetch]);
  for(const [index,controller] of pendingChunks)if(!wantedChunks.has(index)){controller.abort();pendingChunks.delete(index);}
  // Visible batches enter the browser's request queue before speculative ones.
  for(const index of wantedChunks)if(!chunks.has(index))loadChunk(index);
}
function scheduleGrid(){if(!gridFrame)gridFrame=requestAnimationFrame(()=>{gridFrame=null;renderGrid();});}
async function loadChunk(index){
  if(resultsSequence!==sequence||similarMode||pendingChunks.has(index)||failedChunks.has(index))return;
  const ticket=sequence,controller=new AbortController(),parent=searchController.signal,cancel=()=>controller.abort();
  parent.addEventListener('abort',cancel,{once:true});pendingChunks.set(index,controller);
  try{
    const params=new URLSearchParams(searchParams);params.set('offset',offset+index*BATCH_SIZE);params.set('limit',Math.min(BATCH_SIZE,visibleCount-index*BATCH_SIZE));
    const data=await api('/api/automatic/search?'+params,controller.signal);
    if(ticket!==sequence||controller.signal.aborted)return;
    chunks.set(index,data.items);dataVersion++;
    while(chunks.size>12){const old=[...chunks.keys()].find(k=>!wantedChunks.has(k));if(old==null)break;chunks.delete(old);}
    scheduleGrid();
  }catch(error){if(ticket===sequence&&error.name!=='AbortError'){failedChunks.add(index);message('Could not load this section. Search again to retry. '+error.message);}}
  finally{
    parent.removeEventListener('abort',cancel);
    if(pendingChunks.get(index)===controller)pendingChunks.delete(index);
  }
}

function drawHistogram(){
  $('histogram').replaceChildren();const hist=results.histogram;
  if(!hist?.bins){$('hist-min').textContent=$('hist-max').textContent='—';return;}
  const max=Math.max(1,...hist.bins.map(b=>b.count));
  for(let index=0;index<16;index++){
    const count=hist.bins.find(b=>b.bin===index)?.count||0,bar=element('button');bar.style.height=`${Math.max(2,count/max*44)}px`;
    const low=hist.min+(hist.max-hist.min)*index/16,high=hist.min+(hist.max-hist.min)*(index+1)/16;
    bar.title=`${low.toFixed(1)}–${high.toFixed(1)}: ${fmt(count)} occurrences`;bar.setAttribute('aria-label',bar.title);
    bar.onclick=()=>{$('min').value=low.toFixed(3);$('max').value=high.toFixed(3);search();};$('histogram').append(bar);
  }
  $('hist-min').textContent=Number(hist.min).toFixed(2);$('hist-max').textContent=Number(hist.max).toFixed(2);$('hist-label').textContent=$('metric').selectedOptions[0].textContent.toUpperCase()+' DISTRIBUTION';
}
function drawNetworkOutputs(item){
  const direct=item.prediction_structure?.hamza;
  if(direct){
    const host=$('network-evidence-content');host.replaceChildren();$('network-evidence').hidden=false;
    host.append(element('p','Hamza classifier scores','unit-note'));
    const table=element('table',null,'report-table');
    for(const [label,score] of Object.entries(direct.scores).sort((a,b)=>b[1]-a[1])){
      const row=element('tr');row.append(element('td',label),element('td',pct(score)));table.append(row);
    }
    host.append(table);return;
  }
  const evidence=item.networkOutputs,host=$('network-evidence-content');host.replaceChildren();$('network-evidence').hidden=!evidence;
  if(!evidence)return;
  const role=item.raw_prediction_kind,type=item.raw_prediction_type;
  const explanation=evidence.typeApplies&&evidence.selectedTypeScore!=null?
    `${type}: ${pct(evidence.selectedRoleScore)} category score × ${pct(evidence.selectedTypeScore)} type score = ${pct(item.raw_model_confidence)} combined score. Both outputs contribute to the decision.`:
    `This prediction uses the ink role score. Its category score is ${pct(evidence.selectedRoleScore)}.`;
  host.append(element('p',explanation,'unit-note'));
  const pair=item.prediction_structure?.shapePair;
  if(pair)host.append(element('p',`A shape-only classifier distinguished damma from small-high-yeh. It retained their total probability (${pct(pair.preservedPairProbability)}); other type and category scores are unchanged.`,'unit-note'));
  for(const [title,values,chosen,limit]of [['Category classifier',evidence.roles,role,3],['Type classifier',evidence.typeApplies?evidence.types:[],type,5]]){
    if(!values.length)continue;
    host.append(element('strong',title));const table=element('table',null,'report-table');
    const rows=values.slice(0,limit),selected=values.find(v=>v.label===chosen);
    if(selected&&!rows.includes(selected))rows.push(selected);
    for(const v of rows){const row=element('tr');row.append(element('td',(title==='Category classifier'?kinds[v.label]||v.label:v.label)+(v.label===chosen?' · chosen':'')),element('td',pct(v.score)));table.append(row);}host.append(table);
  }
}
async function openDetail(id,entity='item'){
  const ticket=++detailSequence;$('similar').disabled=true;
  message('Opening source context…');
  try{
    const data=await api(`/api/automatic/detail?id=${encodeURIComponent(id)}&entity=${encodeURIComponent(entity)}`);
    if(ticket!==detailSequence)return;
    selection=data;selectedId=id;
    for(const card of $('cards').querySelectorAll('.card'))card.classList.toggle('selected',card.dataset.id===id);
    $('selection-empty').hidden=true;$('selected').hidden=false;
    const source=$('source').value,shownKind=source==='model'?selection.prediction_kind:source==='teacher'?selection.teacher_kind:selection.final_kind;
    const shownType=source==='model'?selection.prediction_type:source==='teacher'?selection.teacher_type:selection.final_type;
    $('selected-type').textContent=entity==='subword'?'Subword body':shownType&&shownType!=='unknown'?shownType:kinds[shownKind]||'Unresolved ink';
    $('selected-id').textContent=id;$('word-text').textContent=selection.word?.text||selection.context_text||selection.text||'Text not assigned';
    drawContext(selection,shownKind||selection.final_kind);
    $('context-caption').textContent=`${selection.page_id} · line ${selection.line||'—'} · selected ink highlighted${!selection.word?' · nearby text is context only':''}`;
    const human=selection.human_reviewed,agree=selection.model_agrees;
    const modelFinal=metadata.inference&&entity!=='subword';
    const method=selection.resolution_method;
    $('evidence-badge').textContent=human?'Human-reviewed ink':modelFinal?'Model prediction'+(selection.prediction_guard?' · structural warning':''):method==='visual-body'?`Body recognized · ${selection.subword_id?'text linked':'text unassigned'}`:method==='horizontal-rule'?'Detached-rule geometry proposal':entity==='subword'?'Automatic subword boundary':selection.refined?'Machine refinement · transcript checked':agree===1?'Weak label · visual model agrees':agree===0?'Weak label · model disagrees':'Unresolved / automatic geometry';
    $('evidence-badge').classList.toggle('flagged',!human&&!method&&(agree===0||selection.final_kind==='unknown'));
    const measures=[['Width',`${selection.w} px`],['Height',`${selection.h} px`],['SVG width',`${(selection.w/4).toFixed(2)} units`],['Ink area',selection.area==null?'—':`${fmt(selection.area)} px²`],['Components',selection.components==null?selection.masks.length:selection.components],[selection.prediction_structure?.hamza?'Direct class score':selection.networkOutputs?'Combined model score':'Visual model score',pct(selection.model_confidence)],['Original proposal evidence',pct(selection.quality)],['Original provenance',selection.proposal_origin||selection.origin||'Machine geometry']];
    if(selection.prediction_structure?.hamza)measures.push(['Role',shownKind==='subword-ink'?'Body':kinds[shownKind]||shownKind],['Type',shownType]);
    if(selection.final_source)measures.push(['Final label source',selection.final_source]);
    if(selection.trainingLabelCorrection)measures.push(['Prepared training label',selection.trainingLabelCorrection.action==='exclude'?'Excluded from training':`${selection.trainingLabelCorrection.type} · automatic geometry`]);
    if(human&&selection.review_attribution)measures.push(['Reviewed by',selection.review_attribution.reviewedBy],['Review recorded by',selection.review_attribution.recordedBy],['Review basis',selection.review_attribution.basis==='explicit-user-attestation'?'User confirmed manual review':selection.review_attribution.basis]);
    if(method)measures.push(['Recovery method',method==='visual-body'?'Visual body classifier':'Detached horizontal rule'],['Text assignment',selection.assignment_status||'—']);
    $('measurements').replaceChildren(...measures.flatMap(([name,value])=>[element('dt',name),element('dd',value)]));
    $('teacher-label').textContent=selection.teacher_type&&selection.teacher_type!=='unknown'?selection.teacher_type:kinds[selection.teacher_kind]||'—';
    $('model-label').textContent=selection.prediction_type&&selection.prediction_type!=='unknown'?selection.prediction_type:kinds[selection.prediction_kind]||'—';
    $('raw-model-label').textContent=selection.raw_prediction_type&&selection.raw_prediction_type!=='unknown'?selection.raw_prediction_type:kinds[selection.raw_prediction_kind]||'Not recorded';
    $('final-label').textContent=selection.final_type&&selection.final_type!=='unknown'?selection.final_type:kinds[selection.final_kind]||'—';
    drawNetworkOutputs(selection);
    const issues=selection.issues.filter(s=>s!=='Subword body includes uncut letters and connections');
    if(selection.trainingLabelCorrection){const c=selection.trainingLabelCorrection;issues.push(c.action==='exclude'?c.evidence:c.action==='support'?
      `The automatic training-label program supports ${c.type} using its ${c.family||'native'} source form. Its evidence weight is automatic; this is not a human review.`:c.action==='supply'?
      `The automatic training-label program supplies ${c.type} from detached-rule geometry inside the text area. This is not a human review.`:
      `The automatic training-label program corrected the original ${c.originalType} proposal to ${c.type} using a reciprocal source-geometry check. This is not a human review.`);}
    if(selection.prediction_guard)issues.push(`${metadata.inference?.structuralPolicy==='diagnostic-only'?'Structural warning; prediction retained':'Visual prediction withheld'}: ${selection.prediction_guard} Raw network score: ${pct(selection.raw_model_confidence)}.`);
    else if(selection.prediction_structure?.accepted)issues.push('Triple-dot prediction passed the visual check: no counters, and an upper dot above a joined or near-touching lower pair.');
    if(selection.unresolved_reason)issues.push(`Original unresolved reason: ${reasons[selection.unresolved_reason]||selection.unresolved_reason}`);
    if(method==='visual-body')issues.push('Coarse body recognition does not establish character cuts or a complete subword boundary. The score is not calibrated accuracy.');
    if(method==='horizontal-rule')issues.push('This type is proposed by geometry. The visual model was not trained to recognize sajda bars or margin rules.');
    for(const match of selection.source_diagnostics?.matches||[])issues.push(`Competing proposal ${match.candidateId}: ${match.kind}, ${(match.regionCoverage*100).toFixed(1)}% of region covered.`);
    $('issues').replaceChildren(...issues.map(s=>element('li',s)));
    const reviewUrl=new URL(selection.reviewUrl,location.origin);
    if(source==='model'&&selection.reviewCanSuggest&&['dot','mark','symbol','other'].includes(shownKind)){
      reviewUrl.searchParams.set('suggestRole',shownKind);reviewUrl.searchParams.set('suggestType',shownType||'unknown');
    }
    $('review-link').href=reviewUrl.toString();$('workbench-link').href=selection.workbenchUrl;
    $('similar').disabled=entity==='subword'||!metadata.learning;message('');
  }catch(error){message(error.message);}
}
function drawContext(item,role){
  const page=item.page,box=item.word?[item.word.x,item.word.y,item.word.w,item.word.h]:item.context_bbox||[item.x,item.y,item.w,item.h];
  const pad=22,x=Math.max(0,box[0]-pad),y=Math.max(0,box[1]-pad),w=Math.min(page.width-x,box[2]+pad*2),h=Math.min(page.height-y,box[3]+pad*2);
  const svg=svgElement('svg',{viewBox:`${x} ${y} ${w} ${h}`,preserveAspectRatio:'xMidYMid meet'});
  const image=svgElement('image',{href:item.scopedSourceUrl||(page.source_kind==='frozen-workbench-ink'?`/api/pages/${item.page_id}/source.svg`:`/api/automatic/page.png?page=${item.page_id}`),x:0,y:0,width:page.width,height:page.height});svg.append(image);
  svg.append(svgElement('path',{d:item.maskPath,fill:colours[role]||'#dd8f35','fill-opacity':'.78'}));
  if(item.entity!=='subword')svg.append(svgElement('rect',{x:item.x-3,y:item.y-3,width:item.w+6,height:item.h+6,fill:'none',stroke:'#d29338','stroke-width':'.9','stroke-dasharray':'3 2'}));
  $('context').replaceChildren(svg);
}
async function similar(){
  if(!selection)return;message('Finding neighbours in the learned visual features…');
  const ticket=++sequence;searchController?.abort();searchController=new AbortController();
  try{const data=await api('/api/automatic/similar?id='+encodeURIComponent(selection.id),searchController.signal);if(ticket!==sequence)return;results=data;resultsSequence=ticket;similarMode=true;offset=0;drawResults();message('Similarity uses the learned mask, context and geometry representation.');}catch(error){if(error.name!=='AbortError')message(error.message);}
}
function clearSelection(){detailSequence++;selection=null;selectedId=null;$('selection-empty').hidden=false;$('selected').hidden=true;for(const card of $('cards').querySelectorAll('.card'))card.classList.remove('selected');}
function download(value,name){const url=URL.createObjectURL(new Blob([JSON.stringify(value,null,2)],{type:'application/json'}));const link=document.createElement('a');link.href=url;link.download=name;link.click();setTimeout(()=>URL.revokeObjectURL(url),30000);}
function renderReport(){
  const host=$('report-content');host.replaceChildren();const build=metadata.build,learn=metadata.learning;
  const grid=element('div',null,'report-grid');
  for(const [label,value]of [['Source ink conserved',build.allSourceInkConserved?'100%':'Check needed'],['Model selection rounds',learn?.roundCount||'Running…'],['Human reference available',learn?.humanReferenceAvailable?'Yes':'No']]){
    const card=element('div',null,'report-stat');card.append(element('strong',value),element('span',label));grid.append(card);
  }host.append(grid);
  if(learn){
    host.append(element('p','Current model: '+learn.framework,'report-note'));
    if(learn.humanValidation?.examples||learn.humanTestAfterSelection?.examples){
      host.append(element('h3','Recognition of human-reviewed marks','report-section-title'));
      const current=metadata.inference?.decoderEvaluation?.splits;
      const validation=current?.validation?{examples:current.validation.human.examples,correct:current.validation.human.afterCorrect}:learn.humanValidation;
      const test=current?.test?{examples:current.test.human.examples,correct:current.test.human.afterCorrect}:learn.humanTestAfterSelection;
      host.append(element('p',`Validation: ${validation?.correct||0} / ${validation?.examples||0} correct. Test after model selection: ${test?.correct||0} / ${test?.examples||0} correct. These are the model’s own role and type predictions, before human corrections. A small set of reviewed positives does not measure false positives across the corpus.`,'report-note'));
    }
    if((metadata.inference?.predictionChecks||learn.predictionChecks))host.append(element('h3','Visual triple-dot checks','report-section-title'),element('p',`${fmt((metadata.inference?.predictionChecks||learn.predictionChecks).checkedTripleDots)} raw triple-dot guesses checked; ${fmt((metadata.inference?.predictionChecks||learn.predictionChecks).flaggedTripleDots??learn.predictionChecks?.withheldTripleDots)} flagged because their source masks do not have the required connected pieces and layout. Raw guesses are retained in details. Training includes ${fmt(learn.hardNegativeBodies?.['isolated-heh-body'])} heh-shaped bodies and ${fmt(learn.hardNegativeBodies?.['small-kaf-body'])} small kaf-body strokes from clean train-page proposals.`,'report-note'));
    const historical=metadata.inference?.decoder?.version&&(learn.decoder?.version||'independent-head-argmax-v0')!==metadata.inference.decoder.version;
    host.append(element('h3',(historical?'Training-time':'Held-out')+' label agreement · mostly automatic references','report-section-title'));
    if(historical)host.append(element('p','These metrics were recorded when the model was trained, using its earlier evaluation rule. The current decoder checks appear below. The weights have not been retrained.','report-note'));
    const table=element('table',null,'report-table'),head=element('tr');
    for(const text of ['Round','Train examples','Role F1','Type F1','Selected'])head.append(element('th',text));table.append(head);
    for(const round of learn.rounds){const tr=element('tr');for(const text of [round.round,fmt(round.trainingExamples),pct(round.validation.role.macroF1),pct(round.validation.type.macroF1),round.round===learn.selectedRound?'✓':''])tr.append(element('td',text));table.append(tr);}host.append(table);
    const inv=learn.inventory;if(inv){host.append(element('h3','Independent transcript constraints','report-section-title'));
    host.append(element('p',`${fmt(inv.changedInstances)} type assignments changed across ${fmt(inv.changedSubwords)} subwords. Inventory count discrepancy: ${fmt(inv.countErrorBefore)} → ${fmt(inv.countErrorAfter)}. Masks and source ink were preserved.`,'report-note'));}
    const selected=learn.rounds.find(r=>r.round===learn.selectedRound);
    host.append(element('h3','Type evaluation coverage','report-section-title'));
    const evaluated=selected.validation.type.support.filter(n=>n>0).length;
    host.append(element('p',`Validation references cover ${evaluated} of ${learn.types.length} model types. The type F1 score excludes types with no eligible validation references; their accuracy is unmeasured.`,'report-note'));
    const difficult=learn.types.map((name,i)=>({name,f1:selected.validation.type.f1[i],support:selected.validation.type.support[i]})).sort((a,b)=>(!a.support?-1:!b.support?1:a.f1-b.f1)||a.support-b.support).slice(0,12);
    const types=element('table',null,'report-table');const tr=element('tr');for(const text of ['Type','Validation examples','Agreement F1'])tr.append(element('th',text));types.append(tr);
    for(const item of difficult){const row=element('tr');for(const text of [item.name,fmt(item.support),item.support?pct(item.f1):'Not evaluated'])row.append(element('td',text));types.append(row);}host.append(types);
    host.append(element('p',learn.architecture,'report-note'),element('p',learn.trainingRule,'report-note'),element('p',learn.testPolicy,'report-note'));
    if(learn.testAfterSelection)host.append(element('p',`Training-time test evaluation, after selecting the model: role F1 ${pct(learn.testAfterSelection.role.macroF1)}, type F1 ${pct(learn.testAfterSelection.type.macroF1)} against automatic reference labels.`,'report-note'));
  }else host.append(element('p','Training has not completed yet. The source-mask corpus can already be queried.','report-note'));
  if(metadata.inference)host.append(element('h3','Model-generated final labels','report-section-title'),element('p',metadata.inference.policy+' '+metadata.inference.geometryWarning+' Structural policy: '+metadata.inference.structuralPolicy+'.','report-note'));
  if(metadata.inference?.decoder){
    host.append(element('h3','Current prediction rule','report-section-title'),element('p',metadata.inference.decoder.rule+' '+metadata.inference.decoder.score,'report-note'));
    const validation=metadata.inference.decoderEvaluation?.splits?.validation;
    if(validation)host.append(element('p',`${fmt(validation.examples)} validation regions: agreement with prepared labels ${(validation.beforeJointAccuracy*100).toFixed(3)}% → ${(validation.afterJointAccuracy*100).toFixed(3)}%. ${validation.corrected} prior disagreements corrected; ${validation.regressed} prior agreements changed to disagreements. These references are mostly automatic labels, so this is a regression check, not a measurement of corpus-wide human-verified accuracy.`,'report-note'));
  }
  const resolution=metadata.resolution;
  if(metadata.scope)host.append(element('h3','Quran text area','report-section-title'),element('p',`${fmt(metadata.scope.excludedRegions)} regions outside the Quran text area are excluded, including ${fmt(metadata.scope.surahTitleFrames)} surah title frames and their enclosed text. The original ${fmt(metadata.scope.sourceInkPixels)} ink pixels are accounted for as ${fmt(metadata.scope.includedInkPixels)} included and ${fmt(metadata.scope.excludedInkPixels)} excluded. Opening pages retain their Quran text opening.`,'report-note'));
  if(resolution){
    host.append(element('h3','Recognition without transcript assignment','report-section-title'),element('p',`${fmt(resolution.recovered['body-ink'])} body regions, ${fmt(resolution.recovered['sajda-bar'])} sajda bars and ${fmt(resolution.recovered['margin-rule'])} margin rules recovered. Unresolved regions: ${fmt(resolution.before.items)} → ${fmt(resolution.after.items)}. Body recognition does not supply missing text identities. Source masks and human annotations were preserved.`,'report-note'));
  }
  host.append(element('h3','What remains unresolved','report-section-title'),element('p',`${fmt(metadata.totals.unresolvedInk)} source ink pixels remain unresolved. Character boundaries, letter forms and kashida cuts require a later segmentation phase.`,'report-note'));
  for(const row of metadata.unresolvedReasons||[])host.append(element('p',`${fmt(row.count)} regions · ${fmt(row.pixels)} pixels: ${reasons[row.reason]||'Diagnosis pending.'}`,'report-note'));
}
$('search-form').onsubmit=event=>{event.preventDefault();search();};
$('kind').onchange=()=>{updateTypes();search();};
for(const id of ['type','quality','reason','split','metric','order','page-size'])$(id).onchange=()=>{updateSourceNote();search();};
$('source').onchange=()=>{updateTypes();search();if(selection)openDetail(selection.id,selection.entity);};
$('same-scale').onchange=()=>{gridSignature='';renderGrid();};
$('reset-filters').onclick=()=>{$('search-form').reset();$('page-size').value='1000';updateTypes();clearSelection();search();};
$('refresh').onclick=async()=>{metadata=await api('/api/automatic/metadata');updateTypes();search(false);};
for(const id of ['previous','previous-top'])$(id).onclick=()=>{offset=Math.max(0,offset-Number($('page-size').value));search(false);};
for(const id of ['next','next-top'])$(id).onclick=()=>{offset+=Number($('page-size').value);search(false);};
document.querySelector('.results').addEventListener('scroll',scheduleGrid,{passive:true});
window.addEventListener('scroll',scheduleGrid,{passive:true});
window.addEventListener('resize',scheduleGrid);
let gridWidth=0;new ResizeObserver(entries=>{const width=entries[0].contentRect.width;if(Math.abs(width-gridWidth)>1){gridWidth=width;scheduleGrid();}}).observe($('cards'));
$('clear-detail').onclick=clearSelection;$('similar').onclick=similar;
$('download').onclick=()=>selection&&download(selection,selection.id.replaceAll(':','-')+'.json');
$('show-report').onclick=()=>{renderReport();$('report').showModal();};$('close-report').onclick=()=>$('report').close();
$('download-report').onclick=()=>download(metadata,'mushaf-automatic-report.json');
for(const button of document.querySelectorAll('.quick-questions button'))button.onclick=()=>{
  $('kind').value=button.dataset.kind;updateTypes();$('type').value=button.dataset.type||'all';$('quality').value=button.dataset.quality||'supported';$('q').value='';search();
};
try{
  metadata=await api('/api/automatic/metadata');$('total-pages').textContent=fmt(metadata.totals.pages);$('total-words').textContent=fmt(metadata.words);$('total-subwords').textContent=fmt(metadata.subwords);$('total-regions').textContent=fmt(metadata.totals.regions??metadata.build.items);
  $('run-state').textContent=metadata.learning?`Model round ${metadata.learning.selectedRound} · local`:`${metadata.status.stage} · local`;
  const params=new URLSearchParams(location.search);
  for(const [key,id]of Object.entries(paramIds))if(params.has(key))$(id).value=params.get(key);
  updateTypes();if(params.has('type')&&[...$('type').options].some(o=>o.value===params.get('type')))$('type').value=params.get('type');
  updateSourceNote();
  await search(false);
  if(params.has('item'))await openDetail(params.get('item'));
}catch(error){message(error.message);$('result-count').textContent='Corpus unavailable';$('run-state').textContent='Build the corpus first';}
