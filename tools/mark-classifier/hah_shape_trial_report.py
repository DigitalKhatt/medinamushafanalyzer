"""Build a lazy-loading visual review for the isolated hah shape trial."""
import argparse
from collections import defaultdict
import json
from pathlib import Path

HTML = r'''<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Medial ح · shape trial</title>
<style>
:root{font:15px system-ui,sans-serif;color:#24333e;background:#f4f7f8}*{box-sizing:border-box}body{margin:0}header,main{max-width:1540px;margin:auto;padding:22px 28px}header{padding-bottom:8px}h1{font-size:27px;margin:0 0 8px}h2{font-size:17px;margin:0}p{line-height:1.5;margin:8px 0}.muted{color:#5d707b}.badge{font-size:12px;letter-spacing:.05em;background:#fff1d1;color:#71500a;border-radius:5px;padding:5px 8px;vertical-align:middle;margin-left:12px}.controls{display:flex;gap:12px;flex-wrap:wrap;align-items:end;padding:15px 0}label{display:grid;gap:5px;font-size:12px;font-weight:650}select,button{font:inherit;background:white;border:1px solid #bdcbd0;border-radius:7px;padding:8px 10px;color:inherit}button{cursor:pointer}select#word{max-width:520px}button:hover{background:#e7f4f3}.facts{display:flex;gap:30px;flex-wrap:wrap;font-size:13px}.facts b{font-size:20px}.notice{padding:12px 16px;background:#e7f2f1;border-left:4px solid #157b78;margin-bottom:14px;line-height:1.5}.grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px}.card{background:white;border:1px solid #d4dfe3;border-radius:10px;padding:16px;overflow:hidden}.figure{height:285px;width:100%;display:block;background:white;border-radius:7px}.figure svg{width:100%;height:100%}.figure canvas{width:100%;height:100%;object-fit:contain;image-rendering:pixelated}.caption{font-size:13px;min-height:40px;margin:10px 0 0;color:#536872}.details{display:grid;grid-template-columns:1.5fr 1fr;gap:14px;margin-top:14px}.swatch{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:4px}.controls label.inline{display:flex;align-items:center;font-size:14px}table{border-collapse:collapse;font-size:13px;width:100%}th,td{text-align:left;padding:6px;border-bottom:1px solid #e4ebee}.error{color:#ab2838}#prototype{height:140px}#wordTitle{font-size:18px;margin:2px 0 12px}#candidate{width:100%}details{margin-top:10px}summary{cursor:pointer}.small{font-size:12px}.legend{font-size:12px;margin:10px 0;display:flex;gap:12px;flex-wrap:wrap}@media(max-width:950px){.grid,.details{grid-template-columns:1fr}.figure{height:260px}header,main{padding:18px}}
@media(min-width:681px) and (max-width:1100px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}.grid .card:last-child{grid-column:1/-1}.details{grid-template-columns:1.3fr 1fr}.figure{height:235px}}
</style>
<header><h1>Medial ح · shape trial <span class="badge">EXPERIMENT · NOT LIVE</span></h1>
<p class="muted">Compare the whole letter shape and its two boundaries. The same body family covers ح، ج، خ.</p>
<div id="facts" class="facts"></div></header>
<main><div id="summary" class="notice">Loading experiment…</div>
<div class="controls">
<label>Page<select id="page"></select></label><label>Show<select id="filter"><option value="all">All examples</option><option value="changed">Different from native cuts</option><option value="unresolved">Native partial / unresolved</option><option value="human">Human-reviewed evaluation</option></select></label>
<label>Subword<select id="word"></select></label><button id="previous" title="Previous example">← Previous</button><button id="next" title="Next example">Next →</button>
<label class="inline"><input id="landmarks" type="checkbox">Contour landmarks</label></div>
<div id="wordTitle"></div><div id="status" class="notice"></div>
<div class="grid">
 <section class="card"><h2>Current template</h2><div id="baseline" class="figure"></div><p id="baselineCaption" class="caption"></p></section>
 <section class="card"><h2>Shape-ranked candidate</h2><div id="trial" class="figure"></div><p id="trialCaption" class="caption"></p></section>
 <section class="card"><h2 id="thirdTitle">Difference in body ink</h2><div id="difference" class="figure"></div><p id="differenceCaption" class="caption"></p></section>
</div>
<div class="legend"><span><i class="swatch" style="background:#147d81"></i>Letter body</span><span><i class="swatch" style="background:#ed7e35"></i>Cut boundary</span><span><i class="swatch" style="background:#cc4e76"></i>Route anchor</span><span><i class="swatch" style="background:#c6d0d8"></i>Other source ink</span></div>
<div class="details"><section class="card"><h2>Inspect alternatives</h2><p class="small muted">Changing this selection only changes the preview. It does not save a label.</p>
<label>Target letter<select id="target"></select></label><label style="margin-top:10px">Candidate (lower distance is closer)<select id="candidate"></select></label>
<div id="terms"></div><details><summary>Intrinsic measurements and generating routes</summary><div id="features"></div></details></section>
<section class="card"><h2>Closest reference shape</h2><div id="prototype" class="figure"></div><div id="prototypeCaption" class="caption"></div>
<p class="small muted">References are automatic template masks from training pages. The current page is excluded. Similarity is not a probability of correctness.</p></section></div>
<details class="card" style="margin-top:14px"><summary>What this experiment does</summary><p>It proposes two cuts around a possible medial hah using six existing contour routes, broadens the anchor search, then ranks the resulting bodies by contour similarity, stroke-relative proportions, concavity, counters, and boundary placement within the body. All candidate cuts come from the source contour. Other letters do not supply endpoint landmarks.</p><p>For a subword with multiple medial hah letters, it selects an ordered nonoverlapping combination. The transcript supplies the number of target letters, not the cut locations. An unrepresented shape or a cut not generated by a route can still be missed.</p><p>Native-template agreement is a proxy. Only the one reviewed mask on page 590 is an independent human comparison. No production template, source artwork, human annotation, or trained neural model was changed.</p></details>
</main>
<script type="module">
const $=id=>document.getElementById(id),esc=x=>String(x).replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('"','&quot;'),pct=x=>x==null?'—':(100*x).toFixed(1)+'%';
let index,word,comparison,candidate,prototypes,pageCache=new Map(),generation=0,loadGeneration=0;
const pathD=p=>p.commands.map(a=>a.join(' ')).join(' ');
function path(p,fill,opacity=1){return p?`<path d="${pathD(p)}" fill="${fill}" fill-rule="${p.fillRule}" opacity="${opacity}"/>`:''}
function line(a,b,color='#ed7e35'){return `<line x1="${a[0]}" y1="${a[1]}" x2="${b[0]}" y2="${b[1]}" stroke="${color}" stroke-width=".15"/>`}
function cut(c){return c?line(c.top.xy,c.bottom.xy):''}
function svg(ink,body,extra='',box=word.bounds){const [x,y,w,h]=box,p=Math.max(w,h)*.08;return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${x-p} ${y-p} ${w+2*p} ${h+2*p}">${path(ink,'#c6d0d8')}${path(body,'#147d81')}${extra}</svg>`}
function baselineCuts(idx){const incoming=word.joins.find(j=>j.afterBody===idx),outgoing=word.joins.find(j=>j.afterBody===idx+1);let extra='';if(incoming){const q=(incoming.endpoints||[incoming.anchor]).at(-1);extra+=line(...q)}if(outgoing){const q=(outgoing.endpoints||[outgoing.anchor])[0];extra+=line(...q)}return extra}
function marks(){if(!$('landmarks').checked)return '';return word.landmarks.filter(p=>p.path===0&&(p.sharp||p.xExtremum||p.yExtremum)).map(p=>`<circle cx="${p.xy[0]}" cy="${p.xy[1]}" r=".07" fill="#bd335c"/><text x="${p.xy[0]+.10}" y="${p.xy[1]-.12}" font-size=".50" fill="#93345b">${p.time.toFixed(2)}</text>`).join('')}
async function loadImage(s){if(!s)return null;let im=new Image();im.src='data:image/png;base64,'+s;await im.decode();return im}
async function differences(base,trial,human){const ticket=++generation;const images=await Promise.all([loadImage(word.sourceMask),loadImage(base?.mask),loadImage(trial?.mask),loadImage(human)]);if(ticket!==generation)return;
 const [x,y,w,h]=word.rasterBounds;const canvas=document.createElement('canvas');canvas.width=w;canvas.height=h;const ctx=canvas.getContext('2d');const arrays=images.map(im=>{ctx.clearRect(0,0,w,h);if(im)ctx.drawImage(im,0,0);return ctx.getImageData(0,0,w,h).data});const out=ctx.createImageData(w,h);
 for(let i=0;i<w*h;i++){let source=arrays[0][4*i]>127,a=arrays[1][4*i]>127,b=arrays[2][4*i]>127,manual=arrays[3][4*i]>127;let rgb=[255,255,255];if(source)rgb=[216,224,229];if(human){if(manual)rgb=[139,73,167];if(b&&manual)rgb=[65,76,93];if(b&&!manual)rgb=[0,158,146]}else{if(a&&b)rgb=[65,76,93];else if(b)rgb=[0,158,146];else if(a)rgb=[234,115,99]}out.data.set([...rgb,255],4*i)}ctx.putImageData(out,0,0);$('difference').replaceChildren(canvas)}
function render(){const targets=word.comparisons,ti=Number($('target').value)||0;comparison=targets[ti];const ci=$('candidate').value;candidate=word.candidates.find(c=>c.id===Number(ci));const idx=comparison.baselineMaskIndex,base=idx==null?null:word.bodies[idx];
 $('wordTitle').textContent=`${word.id} · ${word.text} · ${word.split} page`;
 $('baseline').innerHTML=svg(word.ink,base,(idx==null?'':baselineCuts(idx))+marks());
 let extra=candidate?cut(candidate.right)+cut(candidate.left):'';if(candidate){let a=candidate.routes[0].anchor.xy;extra+=`<circle cx="${a[0]}" cy="${a[1]}" r=".16" fill="#cc4e76"/>`}
 $('trial').innerHTML=svg(word.ink,candidate,extra+marks());
 $('baselineCaption').textContent=`${comparison.baselineForm||'No matched form'} · ${comparison.baselineStatus}${base?'':' · no reliably indexed native mask in this partial word'}`;
 $('trialCaption').textContent=candidate?`Candidate ${candidate.id} · distance ${candidate.distance.toFixed(4)} · ${candidate.conservation.valid?'ink check passed':'ink check failed'}`:'No compatible candidate set';
 const chosen=candidate?.id===comparison.selected;$('status').textContent=`${comparison.status.replaceAll('-',' ')}. ${chosen?'Showing the automatic selection.':'Showing a manually selected alternative.'} All results require review; none are live labels.`;
 let manual=word.humanMask&&ti===0?word.humanMask:null;
 $('thirdTitle').textContent=manual?'Trial vs human-reviewed body':'Difference in body ink';
 $('differenceCaption').textContent=manual?`Automatic choice IoU: ${pct(word.humanMetrics.trialIoU)}; native IoU: ${pct(word.humanMetrics.baselineIoU)}. Purple: reviewed only; green: trial only; dark: shared.`:`Green: trial only. Salmon: native only. Dark: shared. Automatic choice/native IoU: ${pct(comparison.nativeAgreementIoU)}.`;
 differences(base,candidate,manual);
 if(!candidate){$('terms').textContent='No candidate';$('prototype').innerHTML='';return}
 $('terms').innerHTML=`<table><thead><tr><th>Distance term</th><th>Value</th><th>Weight</th></tr></thead><tbody>${Object.entries(candidate.terms).map(([k,v])=>`<tr><td>${esc(k)}</td><td>${v.toFixed(4)}</td><td>${({outline:.65,structure:.25,ports:.10,topology:1})[k]}</td></tr>`).join('')}</tbody></table>`;
 $('features').innerHTML=`<p>${candidate.routes.map(r=>`${esc(r.template)} (${r.originalAnchorChecks?'original anchor checks':'expanded anchor search'})`).join('<br>')}</p><p>Estimated local stroke: ${candidate.stroke.toFixed(3)} native units · counters: ${candidate.holes} · components: ${candidate.components}</p><table>${Object.entries(candidate.features).map(([k,v])=>`<tr><td>${esc(k)}</td><td>${v.toFixed(4)}</td></tr>`).join('')}</table>`;
 const proto=prototypes[candidate.prototype];$('prototype').innerHTML=proto?svg(proto.body,proto.body,'',proto.body.bounds):'';$('prototypeCaption').textContent=`${candidate.prototype} · ${candidate.prototypeForm} · automatic reference`;
 history.replaceState(null,'','#'+word.id+':letter'+comparison.letterIndex);
}
function candidateOptions(){comparison=word.comparisons[Number($('target').value)||0];$('candidate').innerHTML=word.candidates.slice().sort((a,b)=>a.distance-b.distance).map(c=>`<option value="${c.id}">${c.id===comparison.selected?'★ ':''}Candidate ${c.id} · ${c.distance.toFixed(4)} · ${esc(c.prototypeForm)}${c.conservation.valid?'':' · rejected ink check'}</option>`).join('');if(comparison.selected!=null)$('candidate').value=comparison.selected;render()}
async function chooseWord(letter){const ticket=++loadGeneration,key=$('word').value;++generation;if(!key){word=null;$('wordTitle').textContent='No matching examples on this page';$('status').textContent='Choose another page or filter.';for(const id of ['baseline','trial','difference','baselineCaption','trialCaption','differenceCaption','terms','prototype','prototypeCaption','features','target','candidate'])$(id).innerHTML='';return}const page=Number($('page').value);if(!pageCache.has(page)){const response=await fetch(`pages/page${String(page).padStart(3,'0')}.json`);if(!response.ok)throw Error('Page export missing');pageCache.set(page,await response.json())}if(ticket!==loadGeneration)return;word=pageCache.get(page).find(w=>w.id===key);$('target').innerHTML=word.comparisons.map((c,i)=>`<option value="${i}">Letter ${c.letterIndex} · ${esc(c.character)} · ${esc(c.status)}</option>`).join('');const target=word.comparisons.findIndex(c=>c.letterIndex===letter);if(target>=0)$('target').value=target;candidateOptions()}
function options(prefer,letter){let rows=index.words.filter(w=>w.page===Number($('page').value));const filter=$('filter').value;if(filter==='changed')rows=rows.filter(w=>w.comparisons.some(c=>c.nativeAgreementIoU!=null&&c.nativeAgreementIoU<.98));if(filter==='unresolved')rows=rows.filter(w=>!w.complete);if(filter==='human')rows=rows.filter(w=>w.human);
 $('word').innerHTML=rows.map(w=>`<option value="${w.id}">${w.id.split(':')[1]} · ${esc(w.text)}${w.human?' · human review':''}</option>`).join('');$('previous').disabled=$('next').disabled=!rows.length;if(rows.some(w=>w.id===prefer))$('word').value=prefer;chooseWord(letter).catch(error)}
function error(e){$('status').textContent=String(e);$('status').classList.add('error');console.error(e)}
async function init(){index=await(await fetch('index.json')).json();prototypes=await(await fetch('prototypes.json')).json();const s=index.summary,h=s.humanEvaluation[0];$('facts').innerHTML=`<span><b>${index.words.reduce((n,w)=>n+w.comparisons.length,0)}</b> medial letters</span><span><b>39</b> pages</span><span><b>${index.prototypeCount}</b> automatic references</span><span><b>1</b> human-reviewed mask</span>`;
 $('summary').textContent=`On the reviewed page 590 body, native IoU is ${pct(h.baselineIoU)} and this trial is ${pct(h.trialIoU)}. One mask is insufficient to establish general accuracy. Open alternatives to distinguish candidate-generation failures from ranking failures.`;
 $('page').innerHTML=[...new Set(index.words.map(w=>w.page))].map(p=>`<option value="${p}">Page ${p}</option>`).join('');const rawHash=decodeURIComponent(location.hash.slice(1)),hash=rawHash.replace(/:letter\d+$/,'');const wanted=index.words.find(w=>w.id===hash)||index.words.find(w=>w.human)||index.words[0];$('page').value=wanted.page;options(wanted.id,Number(rawHash.match(/:letter(\d+)$/)?.[1]));
 $('page').onchange=()=>options();$('filter').onchange=()=>options();$('word').onchange=()=>chooseWord().catch(error);$('target').onchange=()=>word&&candidateOptions();$('candidate').onchange=()=>word&&render();$('landmarks').onchange=()=>word&&render();
 for(const [id,delta] of [['previous',-1],['next',1]])$(id).onclick=()=>{const s=$('word');s.selectedIndex=(s.selectedIndex+delta+s.length)%s.length;chooseWord().catch(error)};
}init().catch(error);
</script></html>'''

def build(folder):
    report=json.loads((folder/'report.json').read_text());model=json.loads((folder/'model.json').read_text())
    review=folder/'review';(review/'pages').mkdir(parents=True,exist_ok=True)
    pages=defaultdict(list);index=[]
    for word in report['words']:
        index.append({k:word[k] for k in ('id','page','split','text','complete','comparisons')})
        index[-1]['human']='humanMetrics' in word
        for body in word['bodies']:body.pop('normalizedMask',None)
        for candidate in word['candidates']:
            candidate.pop('normalizedMask',None);candidate.pop('partitionMasks',None)
        pages[word['page']].append(word)
    for page,words in pages.items():(review/'pages'/f'page{page:03}.json').write_text(json.dumps(words,ensure_ascii=False,separators=(',',':')))
    prototypes={p['id']:{'body':{k:v for k,v in p['body'].items() if k not in ('mask','normalizedMask')}} for p in model['prototypes']}
    (review/'prototypes.json').write_text(json.dumps(prototypes,ensure_ascii=False,separators=(',',':')))
    (review/'index.json').write_text(json.dumps({'words':index,'prototypeCount':len(prototypes),'summary':{k:v for k,v in report.items() if k!='words'}},ensure_ascii=False,separators=(',',':')))
    (review/'index.html').write_text(HTML)
    print(review/'index.html')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--folder',type=Path,required=True)
    build(parser.parse_args().folder)
