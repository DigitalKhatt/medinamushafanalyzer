"""Build a local visual comparison without changing human annotations."""
import argparse
import base64
from collections import defaultdict
import io
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.optimize import linear_sum_assignment

from letter_retest import metric, summarize
from letter_retest_data import weak_partition

COLORS = ['#326baf','#178a5b','#a64796','#be5b28','#7c6d26','#307b84','#6f49a4','#cf6c89','#5a7e35','#547bba']


def aligned_colors(truth, pred):
    a,b=np.unique(truth[truth>0]),np.unique(pred[pred>0]);mapping={}
    if len(a) and len(b):
        inter=np.array([[( (truth==i)&(pred==j)).sum() for j in b] for i in a])
        rows,cols=linear_sum_assignment(-inter)
        mapping={int(b[c]):int(a[r]) for r,c in zip(rows,cols)}
    for i in b:
        if int(i) not in mapping:mapping[int(i)]=int(a.max(initial=0))+len(mapping)+1
    return mapping


def colored(labels, ink, roles=None, mapping=None):
    h,w=labels.shape;image=Image.new('RGB',(w,h),'white');pixels=np.asarray(image).copy()
    pixels[ink & (labels==0)]=[155,155,155]
    for i in np.unique(labels[labels>0]):
        color='#edbd48' if roles is not None and roles[i]==2 else COLORS[((mapping or {}).get(int(i),int(i))-1)%len(COLORS)]
        pixels[labels==i]=tuple(int(color[j:j+2],16) for j in (1,3,5))
    return Image.fromarray(pixels)


def data_url(image):
    b=io.BytesIO();image.save(b,format='PNG')
    return 'data:image/png;base64,'+base64.b64encode(b.getvalue()).decode()


def main(folder, data):
    dataset=json.loads((folder/'dataset.json').read_text())
    reports={name:json.loads((folder/name/'evaluation.json').read_text()) for name in ('mask2former','segformer')}
    lookup={name:{r['id']:r for r in report['examples']} for name,report in reports.items()}
    rows=[];body_metrics=defaultdict(list);kashida_metrics=defaultdict(list);panels=[]
    old=data/'automatic/experiments/letter-cut-benchmark'
    legacy={'mask2former':np.load(old/'validation-instance-masks.npz'),
            'segformer':np.load(old/'validation-segformer-hires-masks.npz')}
    baseline=defaultdict(list);teacher_scores=[];teacher_examples=[]
    for record in dataset['records']:
        if record['split']=='train':continue
        with np.load(folder/record['file']) as f:truth,roles=f['instances'],f['roles']
        ink=truth>0
        row={k:record.get(k) for k in ('id','page','group','provenance','textForDisplayOnly','regions','bodyRegions','kashidaRegions')}
        row['images']={'input':data_url(Image.fromarray(np.where(ink,0,255).astype('uint8'))),
                       'reference':data_url(colored(truth,ink,roles))}
        display=[Image.fromarray(np.where(ink,0,255).astype('uint8')),colored(truth,ink,roles)]
        row['metrics']={};row['rawMetrics']={}
        for name in reports:
            info=lookup[name][record['id']]
            with np.load(folder/name/info['file']) as f:pred,pr,raw=f['instances'],f['roles'],f['raw']
            pic=colored(pred,ink,pr,aligned_colors(truth,pred))
            display.append(pic);row['images'][name]=data_url(pic);row['metrics'][name]=summarize([info['metrics']])
            row['images'][name+'_raw']=data_url(colored(raw,ink,pr,aligned_colors(truth,raw)))
            row['rawMetrics'][name]=summarize([info['rawMetrics']])
            for role,group in ((1,body_metrics),(2,kashida_metrics)):
                group[(name,record['group'])].append(metric(np.where(roles[truth]==role,truth,0),np.where(pr[pred]==role,pred,0)))
            if record['group']=='human_validation':
                key=record['page']+'_mask2former' if name=='mask2former' else record['page']
                previous=legacy[name][key]
                reference=truth[3:-3,3:-3]
                if previous.shape!=reference.shape:raise ValueError('Legacy comparison crop mismatch')
                baseline[name].append(metric(reference,previous))
        if record['group']=='human_validation':
            vector=json.loads((folder/'vector'/f"{record['page']}.json").read_text())
            source=next(x for x in vector['examples'] if x['id']==record['id'])
            predicted=np.zeros_like(truth);pred_roles=np.zeros(1,'uint8');error=None
            try:
                labels,pred_roles,bbox,_,_=weak_partition(source,vector['raster']['width'])
                dx,dy=bbox[0]-record['bbox'][0],bbox[1]-record['bbox'][1]
                x0,y0=max(0,dx),max(0,dy);x1,y1=min(truth.shape[1],dx+labels.shape[1]),min(truth.shape[0],dy+labels.shape[0])
                if x1>x0 and y1>y0:predicted[y0:y1,x0:x1]=labels[y0-dy:y1-dy,x0-dx:x1-dx]
            except ValueError as e:error=str(e)
            score=None if error else metric(truth,predicted,roles,pred_roles)
            if score is not None:teacher_scores.append(score)
            teacher_examples.append(dict(id=record['id'],complete=source['complete'],error=error,metrics=score))
        rows.append(row)
        if record['group']=='human_validation':panels.append((record['id'],display,row['metrics']))
    for f in legacy.values():f.close()
    summary={'datasetGroups':dataset['groups'],'reviewedBaselineRecomputed':{n:summarize(v) for n,v in baseline.items()},
             'baselineDefinitions':{'mask2former':'Six reviewed training examples, frozen encoder, 48 steps',
                                    'segformer':'Higher-resolution six-example B2 pilot, frozen encoder, 96 steps per fold'},
             'currentTemplateCutter':{'totals':summarize(teacher_scores),'evaluatedExamples':len(teacher_scores),
                                     'note':'Diagnostic only: crops failing native raster checks have no score; this subset is not comparable to the full six-word model evaluation.',
                                     'examples':teacher_examples},
             'models':{n:{'groups':r['groups'],'rawGroups':r['rawGroups'],'settings':r['settings']} for n,r in reports.items()},
             'bodyOnly':{n:{g:summarize(v) for (m,g),v in body_metrics.items() if m==n} for n in reports},
             'kashidaOnly':{n:{g:summarize(v) for (m,g),v in kashida_metrics.items() if m==n} for n in reports}}
    (folder/'comparison.json').write_text(json.dumps(summary,indent=2)+'\n')
    (folder/'report-data.json').write_text(json.dumps(rows,ensure_ascii=False,separators=(',',':'))+'\n')
    font_path='/System/Library/Fonts/Supplemental/Arial.ttf'
    font=ImageFont.truetype(font_path,19);small=ImageFont.truetype(font_path,15)
    image=Image.new('RGB',(1440,110+len(panels)*240),'#f5f7fa');d=ImageDraw.Draw(image)
    d.text((24,15),'Reviewed validation words · image-only inference',fill='#202b38',font=font)
    for col,title in enumerate(['Input ink','Human-reviewed cuts','Mask2Former','SegFormer-B2 + decoder']):d.text((24+360*col,60),title,fill='#202b38',font=font)
    for idx,(identifier,pics,stats) in enumerate(panels):
        top=100+idx*240;d.text((24,top),identifier,fill='#202b38',font=small)
        for col,pic in enumerate(pics):
            scale=min(320/pic.width,165/pic.height);pic=pic.resize((round(pic.width*scale),round(pic.height*scale)),Image.Resampling.NEAREST)
            image.paste(pic,(24+col*360+(320-pic.width)//2,top+28+(165-pic.height)//2))
            if col>=2:
                s=stats['mask2former' if col==2 else 'segformer']
                d.text((24+col*360,top+205),f"F1@.50 {s['instanceF1AtIoU50']:.1%} · mean IoU {s['meanIoUPerReference']:.1%}",fill='#35465e',font=small)
    image.save(folder/'reviewed-comparison.png')
    html='''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Letter segmentation model comparison</title><style>
body{font:16px system-ui;margin:24px auto;max-width:1450px;padding:0 20px;background:#f5f7fa;color:#202b38}header,article{background:white;border:1px solid #d9e1e8;border-radius:10px;padding:20px;margin-bottom:18px}h1{font-size:26px}h2{font-size:18px}.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:18px}img{width:100%;height:190px;object-fit:contain;image-rendering:pixelated}p{line-height:1.55}select,input,button{font:inherit;padding:8px;margin:4px}table{border-collapse:collapse}td,th{text-align:left;padding:9px;border-bottom:1px solid #d9e1e8}small{color:#53627a}@media(max-width:850px){.grid{grid-template-columns:1fr 1fr}}</style>
<header><h1>Letter segmentation · larger-data retrial</h1><p>Inputs are isolated subword ink images. No transcript, letter count or template landmarks are supplied to either model. Gold is kashida; other colors identify body instances; gray is unassigned ink.</p><div id="summary"></div>
<p>The six reviewed validation words were inspected in earlier work and are a small comparison set. Scores against automatic targets measure agreement with the template cutter, not verified segmentation accuracy. Models and thresholds were selected using a separate validation page, without the six reviewed words or test pages.</p>
<p><a href="reviewed-comparison.png">Six-word comparison image</a> · <a href="comparison.json">Full metrics</a> · <a href="dataset.json">Dataset and provenance</a></p>__PAGE_LINKS__
<label><input type="checkbox" id="raw"> Show raw Mask2Former output before ink-conservation decoding</label>
<label>Set <select id="group"><option value="human_validation">Human-reviewed validation</option><option value="weak_test">Test pages · automatic reference</option><option value="weak_validation">Checkpoint-selection page · automatic reference</option><option value="weak_validation_review_pages">Other crops on reviewed pages · automatic reference</option></select></label>
<label>Find <input id="find" placeholder="page600:l14-w5-s1"></label><label>Sort <select id="sort"><option value="id">Page order</option><option value="mask2former">Lowest Mask2Former IoU</option><option value="segformer">Lowest SegFormer IoU</option></select></label><button id="previous">Previous</button><button id="next">Next</button><span id="count"></span></header><main id="cases"></main>
<script id="records" type="application/json">__ROWS__</script><script id="results" type="application/json">__SUMMARY__</script><script>
const rows=JSON.parse(document.querySelector('#records').textContent),summary=JSON.parse(document.querySelector('#results').textContent);let page=0;const $=s=>document.querySelector(s),pct=x=>(100*x).toFixed(1)+'%';
const table=document.createElement('table');let head=table.insertRow();for(const name of ['Model','F1 @ IoU .50','Mean IoU / reference','Boundary F1 (2 px)']){let th=document.createElement('th');th.textContent=name;head.append(th)}
for(const [name,title] of [['mask2former','Mask2Former'],['segformer','SegFormer-B2']]){for(const [suffix,s] of [[' — previous pilot',summary.reviewedBaselineRecomputed[name]],[' — new run',summary.models[name].groups.human_validation]]){let tr=table.insertRow();for(const text of [title+suffix,pct(s.instanceF1AtIoU50),pct(s.meanIoUPerReference),pct(s.boundaryF1Within2px)])tr.insertCell().textContent=text}}$('#summary').append(table);
function render(){let a=rows.filter(x=>x.group===$('#group').value&&(!$('#find').value||[x.id,x.textForDisplayOnly].join(' ').includes($('#find').value)));const s=$('#sort').value;a.sort(s==='id'?(a,b)=>a.id.localeCompare(b.id):(a,b)=>a.metrics[s].meanIoUPerReference-b.metrics[s].meanIoUPerReference);page=Math.min(page,Math.max(0,Math.ceil(a.length/24)-1));$('#count').textContent=`${a.length} crops · page ${page+1}`;$('#cases').replaceChildren();for(const r of a.slice(page*24,page*24+24)){const card=document.createElement('article');card.id=r.id;const title=document.createElement('h2');title.textContent=r.id+' '+(r.textForDisplayOnly||'');card.append(title);const grid=document.createElement('div');grid.className='grid';for(const [key,name] of [['input','Input ink'],['reference',r.provenance==='human-reviewed'?'Human-reviewed reference':'Automatic reference'],['mask2former','Mask2Former'],['segformer','SegFormer-B2 + decoder']]){const box=document.createElement('div'),p=document.createElement('p'),img=document.createElement('img'),raw=key==='mask2former'&&$('#raw').checked;p.textContent=name+(raw?' — raw':'');img.src=r.images[raw?key+'_raw':key];img.alt=name+' '+r.id;img.loading='lazy';box.append(p,img);if(r.metrics[key]){const m=(raw?r.rawMetrics:r.metrics)[key],small=document.createElement('small');small.textContent=`${m.predictedRegions} regions · F1 ${pct(m.instanceF1AtIoU50)} · IoU ${pct(m.meanIoUPerReference)}`;box.append(small)}grid.append(box)}card.append(grid);$('#cases').append(card)}}
for(const id of ['group','find','sort','raw'])$('#'+id).addEventListener('input',()=>{page=0;render()});$('#previous').onclick=()=>{page=Math.max(0,page-1);render()};$('#next').onclick=()=>{page++;render()};render();
</script></html>'''
    # Self-contained so the report works directly from a local file as well.
    html=html.replace('__ROWS__',json.dumps(rows,ensure_ascii=False,separators=(',',':')).replace('<','\\u003c'))
    html=html.replace('__SUMMARY__',json.dumps(summary,separators=(',',':')).replace('<','\\u003c'))
    page_links=[f'<a href="{p.parent.name}/index.html">Page {int(p.parent.name[4:])}: full-page model / template review</a>'
                for p in sorted(folder.glob('page[0-9][0-9][0-9]/index.html'))]
    html=html.replace('__PAGE_LINKS__','<p>'+' · '.join(page_links)+'</p>' if page_links else '')
    (folder/'report.html').write_text(html)
    print(json.dumps({n:r['groups']['human_validation'] for n,r in reports.items()},indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--folder',type=Path,required=True);p.add_argument('--data',type=Path,default=Path('data'));a=p.parse_args();main(a.folder,a.data)
