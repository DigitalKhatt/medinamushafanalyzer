const $ = s => document.querySelector(s);
const make = (tag, text, cls) => { const e = document.createElement(tag); if(text!=null)e.textContent=text; if(cls)e.className=cls; return e; };
const filterIds = ['q','page','char','form','class','type','status','sort'];
const state = {kind:'letters',offset:0,limit:36,selected:null,detail:null,request:0};
let metadata;
function fail(error) { $('#error').textContent=error.message||String(error); $('#error').hidden=false; }
async function api(path, options) { const response=await fetch(path,options); const body=await response.json(); if(!response.ok)throw new Error(body.error||response.statusText); return body; }
function imageURL(kind,id,context=false) { return '/api/image.svg?'+new URLSearchParams({kind,id,context:context?'1':'0',raw:$('#raw').checked?'1':'0'}); }
function badge(status) { return make('span',status.replaceAll('-',' '),'badge '+status); }
function setOptions(id, values) { const node=$('#'+id); for(const value of values){const option=make('option',value);option.value=value;node.append(option);} }
function updateSummary() {
  const c=metadata.counts;
  $('#subtitle').textContent=`${metadata.pages.map(p=>'Page '+Number(p.id.slice(4))).join(' · ')} · ${c.words} words · ${c.transcriptTypedMarks}/${c.marks} marks mapped to a transcript type · ${c.classifierDifferences} classifier differences`;
  $('#summary').replaceChildren();
  for(const [n,label,warning] of [[c.letters,'transcript letters'],[c.subwords,'subwords'],[c.proposedMarks,'proposed attachments'],[c.unresolvedLetters,'unaligned letters',true],[c.unresolvedMarks,'marks to resolve',true],[c.reviewedMarks,'reviewed attachments']]) {
    const item=make('div',undefined,'stat'+(warning?' warning':''));item.append(make('b',n.toLocaleString()),make('span',label));$('#summary').append(item);
  }
  $('#database').textContent=metadata.corpus;
  $('#provenance').textContent=JSON.stringify({method:metadata.model.method,modelSha256:metadata.model.modelSha256,trainingStep:metadata.model.trainingStep,threshold:metadata.model.threshold,pages:metadata.pages,reviews:metadata.reviews,learnedMarkClassifierUsed:false,note:'The checkpoint was also trained on page 3; this is a case study, not a held-out accuracy estimate. Mark owners use transcript quotas and geometry. Mark types shown in the transcript inventory are expected types, not classifier predictions.'},null,2);
}
function params() { const p=new URLSearchParams({kind:state.kind,limit:state.limit,offset:state.offset});for(const id of filterIds)if($('#'+id).value)p.set(id,$('#'+id).value);return p; }
function updateKinds() {
  $('#status option[value="unresolved"]').textContent=state.kind==='marks'?'Unresolved ownership':'Unresolved body splits';
  for(const button of document.querySelectorAll('[data-kind]'))button.setAttribute('aria-pressed',String(button.dataset.kind===state.kind));
  for(const id of ['char','form','class']){$('#'+id).disabled=state.kind!=='letters';if(state.kind!=='letters')$('#'+id).value='';}
  for(const option of $('#status').options)option.disabled=['reviewed','stale-review','word-sign','type-unresolved','classifier-difference'].includes(option.value)&&state.kind!=='marks';
  if($('#status').selectedOptions[0]?.disabled)$('#status').value='';
}
async function search() {
  const serial=++state.request;$('#error').hidden=true;
  try {
    const result=await api('/api/search?'+params());if(serial!==state.request)return;
    $('#result-count').textContent=`${result.total.toLocaleString()} ${state.kind} · ${result.total?state.offset+1:0}–${Math.min(state.offset+result.rows.length,result.total)}`;
    $('#grid').replaceChildren();
    for(const row of result.rows) {
      const card=make('button',undefined,'card'+(state.selected?.id===row.id?' selected':''));card.type='button';card.dataset.id=row.id;
      const title=make('div',undefined,'card-title'),status=row.effective_status||row.status;
      title.append(make('span',row.char||row.text||row.type,state.kind==='marks'?'muted':'char'),badge(status));
      const img=make('img');img.src=imageURL(state.kind,row.id,$('#context').checked);img.alt=row.id;img.loading='lazy';
      card.append(title,img,make('span',row.id.replace('page','p'),'location'));
      const measure=state.kind==='letters'?`${row.form} · ${row.w==null?'body unresolved':row.w+' × '+row.h+' px'}`:
        state.kind==='subwords'?(row.alignment==='single-base-source'?'Original body · 1 letter':`${row.model_bodies} bodies / ${row.expected_letters} letters`):`${row.expected_type||row.type||row.kind+' · ?'} · ${row.w} × ${row.h} px`;
      card.append(make('span',measure,'measure'));const kind=state.kind;card.onclick=()=>select(kind,row.id);$('#grid').append(card);
    }
    if(!result.rows.length)$('#grid').append(make('p','No matching items. Try another filter.','empty'));
    $('#previous').disabled=state.offset===0;$('#next').disabled=state.offset+state.limit>=result.total;
    $('#pagination').textContent=`${Math.floor(state.offset/state.limit)+1} / ${Math.max(1,Math.ceil(result.total/state.limit))}`;
  } catch(error) { fail(error); }
}
async function select(kind,id) {
  state.selected={kind,id};history.replaceState(null,'','#'+new URLSearchParams({kind,id}));
  for(const card of $('#grid').children)card.classList.toggle('selected',card.dataset.id===id);
  try { const detail=await api('/api/detail?'+new URLSearchParams({kind,id}));if(state.selected.id!==id)return;state.detail=detail;renderDetail(); }
  catch(error){fail(error);}
}
function renderDetail() {
  const data=state.detail;if(!data)return;const selected=data.selected;
  const sid=data.kind==='subwords'?selected.id:selected.subword_id;
  const sub=data.subwords.find(s=>s.id===sid), localLetters=data.letters.filter(l=>l.subword_id===sid);
  const inspector=$('#inspector');inspector.replaceChildren();
  const head=make('div',undefined,'inspector-head');head.append(make('h2',data.word?.text||selected.id,'arabic'),badge(selected.status));
  inspector.append(head,make('span',selected.id,'location'));
  const img=make('img',undefined,'word-image');img.src=imageURL(data.kind,selected.id,true);img.alt='Selected ink in the original word';inspector.append(img);
  inspector.append(make('p','Selected ink is colored; the rest of the word stays gray. Gold shows connecting strokes.','small-note'));
  inspector.append(make('h3','Letters in this word'));const strip=make('div',undefined,'letter-strip');
  for(const l of data.letters){const b=make('button',l.char,'letter-chip '+l.status);b.append(make('small',String(l.word_ordinal)));b.title=l.id+' · '+l.form;b.onclick=()=>select('letters',l.id);strip.append(b);}inspector.append(strip);
  if(sub){
    inspector.append(make('h3','Subword segmentation'));
    const facts=make('dl',undefined,'facts');
    const original=sub.diagnostics.bodyMethod==='original-single-letter';
    for(const [key,val] of [['Text',sub.text],['Body handling',original?'Original ink · segmenter bypassed':'Mask2Former'],['Expected letters / body pieces',`${sub.expected_letters} / ${sub.model_bodies}`],['Identity alignment',sub.alignment],['Unassigned body pixels',sub.diagnostics.unassignedPixels||0],['Completed from raw gaps',sub.diagnostics.rawUnassignedPixels||0]])facts.append(make('dt',key),make('dd',String(val)));
    inspector.append(facts,make('p',original?'The transcript has one base character. Its complete original body is shown unchanged; Mask2Former and its decoder were not called.':sub.status==='unresolved'?'Letter bodies could not be aligned safely. The model masks remain visible without invented letter owners.':'Letter identities are proposed from transcript order after inference. These body cuts have not been human-approved.','description'));
    const button=make('button','Inspect whole subword');button.onclick=()=>select('subwords',sid);inspector.append(button);
  }
  inspector.append(make('h3','Marks & dots in this subword'));
  const table=make('table',undefined,'inventory'),thead=make('tr');for(const h of ['Ink','Transcript mark','Letter','Validation'])thead.append(make('th',h));table.append(thead);
  const marks=data.marks.filter(m=>m.subword_id===sid);
  for(const m of marks){
    const tr=make('tr',undefined,m.owner===selected.id||m.id===selected.id?'focused':'');
    const pic=make('img');pic.src=imageURL('marks',m.id);pic.alt=m.type||m.kind;
    const td=make('td');td.append(pic);tr.append(td);
    const type=make('td'),b=make('button',m.expected_type||m.type||m.kind+' · ?', 'link');b.onclick=()=>reviewForm(m,localLetters);type.append(b);
    type.append(make('div',m.dot_count?`${m.dot_count} dots on ${m.source_character}`:m.character?'◌'+m.character:'ambiguous / unassigned','small-note'));
    if(m.word_text_offset!=null)type.append(make('div',`word character ${m.word_text_offset+1}`,'small-note'));tr.append(type);
    const owner=data.letters.find(l=>l.id===m.owner);tr.append(make('td',owner?`${owner.char} · ${owner.word_ordinal}`:m.status==='word-sign'?'word sign':'—'));
    const stateCell=make('td');stateCell.append(badge(m.effective_status));
    if(m.validation){stateCell.append(make('div',m.validation,'small-note'));if(m.classifier_type)stateCell.append(make('div',`classifier: ${m.classifier_type}`,'small-note'));}
    tr.append(stateCell);table.append(tr);
  }
  inspector.append(table,make('div',undefined,'review-mount'));
  if(!marks.length)inspector.append(make('p','No detached marks in this subword.','small-note'));
  if(data.expected.length){
    inspector.append(make('h3','Expected marks from the transcript'));
    for(const l of localLetters){const counts=new Map();for(const slot of data.expected.filter(s=>s.letter_id===l.id))counts.set(slot.type,(counts.get(slot.type)||0)+1);
      const names=[...counts].map(([name,count])=>`${name} × ${count}`).join(', ')||'none';
      const attached=marks.filter(m=>m.owner===l.id).length;
      inspector.append(make('p',`${l.char} · letter ${l.word_ordinal}: ${names} · ${attached} regions assigned`,'small-note'));}
    inspector.append(make('p','Types and characters are assigned from the transcript and geometry. Classifier outputs are compared afterward and never choose these assignments.','small-note'));
  }
  if(data.kind==='marks'){const mark=data.marks.find(m=>m.id===selected.id);if(mark)reviewForm(mark,localLetters);}
  const details=make('details');details.append(make('summary','Assignment details'));const pre=make('pre',JSON.stringify({subword:sub?.id,alignment:sub?.alignment,diagnostics:sub?.diagnostics,marks:marks.map(m=>({id:m.id,type:m.type,owner:m.owner,method:m.method,margin:m.margin,typeReviewed:Boolean(m.type_reviewed),attachmentReviewed:m.attachment_reviewed}))},null,2));details.append(pre);inspector.append(details);
}
function reviewForm(mark, letters) {
  const mount=$('.review-mount');if(!mount)return;mount.replaceChildren();const box=make('div',undefined,'review-box');
  box.append(make('h3',`Review attachment · ${mark.expected_type||mark.type||mark.kind}`),make('span',mark.id,'location'),make('p',mark.review_stale?'This review is stale because its source or segmentation changed. Inspect it again.':'Choose the letter that owns this physical mark. Ownership is proposed from transcript counts and geometry.'));
  if(mark.transcript_candidates?.length){const candidates=state.detail.expected.filter(s=>mark.transcript_candidates.includes(s.id));box.append(make('p','Transcript candidates: '+candidates.map(s=>`${s.type}${s.character?' ◌'+s.character:''} · character ${s.word_text_offset+1}`).join('; '),'small-note'));}
  const select=make('select');select.setAttribute('aria-label','Owning letter');const empty=make('option','No letter / word-level sign');empty.value='';select.append(empty);
  for(const l of letters){const option=make('option',`${l.char} · letter ${l.word_ordinal} · ${l.form}${l.status==='unresolved'?' (body unresolved)':''}`);option.value=l.id;select.append(option);}select.value=mark.owner||'';
  const note=make('textarea');note.setAttribute('aria-label','Review note');note.placeholder='Optional review note';note.value=mark.review_note||'';box.append(select,note);
  const actions=make('div',undefined,'review-actions'),approve=make('button','Approve attachment','primary'),reopen=make('button','Reopen');reopen.disabled=!mark.review_revision;
  const save=async state=>{approve.disabled=true;reopen.disabled=true;try{
    await api('/api/review',{method:'PUT',headers:{'Content-Type':'application/json','X-Letter-Review-Token':metadata.token},body:JSON.stringify({id:mark.id,fingerprint:mark.fingerprint,revision:mark.review_revision||0,letter_id:select.value||null,state,note:note.value})});
    metadata=await api('/api/metadata');updateSummary();await selectCurrent();await search();
  }catch(error){fail(error);approve.disabled=false;reopen.disabled=!mark.review_revision;}};
  approve.onclick=()=>save('approved');reopen.onclick=()=>save('reopened');actions.append(approve,reopen);box.append(actions,make('p','Saved in the separate review database. Automatic segmentation and existing training annotations are unchanged.','small-note'));mount.append(box);
}
async function selectCurrent(){if(state.selected)await select(state.selected.kind,state.selected.id);}
for(const button of document.querySelectorAll('[data-kind]'))button.onclick=()=>{state.kind=button.dataset.kind;state.offset=0;updateKinds();search();};
$('#filters').onsubmit=event=>{event.preventDefault();state.offset=0;search();};
let debounce;for(const id of filterIds)$('#'+id).addEventListener('input',()=>{clearTimeout(debounce);debounce=setTimeout(()=>{state.offset=0;search();},id==='q'?200:0);});
$('#raw').onchange=()=>{search();renderDetail();};$('#context').onchange=search;
$('#previous').onclick=()=>{state.offset=Math.max(0,state.offset-state.limit);search();};$('#next').onclick=()=>{state.offset+=state.limit;search();};
$('#reset').onclick=()=>{for(const id of filterIds)$('#'+id).value=id==='sort'?'reading':'';state.offset=0;search();};
try {
  metadata=await api('/api/metadata');setOptions('page',metadata.pages.map(p=>p.id));setOptions('char',metadata.characters);setOptions('class',metadata.classes);setOptions('type',metadata.markTypes);
  const initial=new URLSearchParams(location.search);if(['letters','subwords','marks'].includes(initial.get('kind')))state.kind=initial.get('kind');
  for(const id of filterIds)if(initial.has(id))$('#'+id).value=initial.get(id);
  updateSummary();updateKinds();await search();
  const hash=new URLSearchParams(location.hash.slice(1));if(hash.get('id'))await select(hash.get('kind')||'letters',hash.get('id'));
}catch(error){fail(error);}
