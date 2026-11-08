"""Review the two checkpoints on identical source crops and score actual seen–reh connectors."""
from argparse import ArgumentParser
from collections import defaultdict
from html import escape
from pathlib import Path
import re

from letter_seen_reh_trial import (canvas_example, get_model, np, predict, read, save_prediction,
                                 SEEN, torch, write)
from letter_page_compare import source_crop
from letter_template_repeat import colored_mask, raster_runs, sha


def draw(ink, instances, roles, filename):
    # Keep Arabic reading order across models, since query numbers are arbitrary.
    ids = [i for i in range(1,len(roles)) if roles[i] == 1]
    ids.sort(key=lambda i: -float(np.where(instances == i)[1].mean()))
    ids += [i for i in range(1,len(roles)) if roles[i] == 2]
    colors = np.zeros(len(roles),'uint16')
    for j,i in enumerate(ids,1):
        colors[i] = j
    colored_mask(ink,colors[instances],np.array([0,*[roles[i] for i in ids]])).save(filename)


def native_svg(row):
    x,y,w,h=row['bounds']
    def path(region,color):
        d=' '.join(' '.join(map(str,cmd)) for cmd in region['commands'])
        rule='nonzero' if region.get('fillRule')=='nonzero' else 'evenodd'
        return f'<path d="{escape(d)}" fill="{color}" fill-rule="{rule}"/>'
    palette=['#26769f','#9d4881','#2d8560','#b6632b','#6b60a7','#567d2b']
    shapes=[path(row['sourceInk'],'#c3c8ca')]
    shapes.extend(path(b,palette[i%len(palette)]) for i,b in enumerate(row['bodies']))
    shapes.extend(path(j['kashida'],'#e5b63d') for j in row['joins'] if j.get('kashida',{}).get('commands'))
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x-.6} {y-.6} {w+1.2} {h+1.2}">'+''.join(shapes)+'</svg>'


def connector_ids(row, document, roles):
    n = len(row['bodies']); result=[]
    for i,join in enumerate(row['joins']):
        path=join.get('kashida')
        if not path or not path.get('commands'):continue
        if not raster_runs(path,document['raster']['width'],document['raster']['height'],document['pdfToRaster']):continue
        n+=1
        if row['baseText'][i] in SEEN and row['baseText'][i+1]=='ر':result.append(n)
    if n!=len(roles)-1:raise ValueError('Incorrect mapping between native joints and frozen regions')
    return result


def connector_iou(truth,region,pred,roles):
    mask=truth==region
    return max((int(((pred==i)&mask).sum())/int(((pred==i)|mask).sum())
                for i in np.flatnonzero(roles==2)),default=0.)


def preview(folder, records):
    from PIL import Image, ImageDraw, ImageFont
    sid='page069:l4-w7-s2'
    if sid not in records:return
    assets=folder/'gallery/assets'
    stub=sid.replace(':','-')
    with np.load(folder/records[sid]['file']) as source:
        draw(source['instances']>0,source['instances'],source['roles'],assets/(stub+'-reference.png'))
    sheet=Image.new('RGB',(840,154),'white')
    pen=ImageDraw.Draw(sheet)
    try:font=ImageFont.truetype('Helvetica.ttc',17)
    except OSError:font=ImageFont.load_default(size=17)
    for i,(panel,title) in enumerate([('reference','Template reference'),('previous','Previous Mask2Former'),('trial','Fine-tuned Mask2Former')]):
        pen.text((i*280+14,12),title,font=font,fill='#193c46')
        with Image.open(assets/(stub+'-'+panel+'.png')) as source:
            scale=min(244/source.width,98/source.height)
            shape=source.resize((round(source.width*scale),round(source.height*scale)),Image.Resampling.NEAREST)
        sheet.paste(shape,(i*280+(280-shape.width)//2,40+(98-shape.height)//2))
    sheet.save(folder/'gallery/page069-comparison.png')


def render(base,folder,artwork):
    data=read(folder/'dataset.json'); report=read(folder/'evaluation.json');recipe=read(folder/'recipe.json')
    if sha(folder/'dataset.json')!=recipe['datasetSha256']:raise ValueError('Frozen masks changed')
    snapshot=Path(data['nativeSnapshot'])
    native=read(artwork)
    split_lookup={r['id']:r['split'] for r in read(base/'inputs/corpus.json')['pages']}
    records={r['id']:r for r in data['records']}
    metrics={m:{r['id']:r for r in read(folder/(m+'-examples.json'))} for m in ('previous','trial')}
    if metrics['trial'].keys()!=metrics['previous'].keys():raise ValueError('Models evaluated different crops')
    output=folder/'gallery';assets=output/'assets';assets.mkdir(parents=True,exist_ok=True)
    unscored=set(native['openSeenReh'])|{'page109:l5-w8-s2'}
    ids=set(metrics['trial'])|unscored
    illustrations=[];refs={}; connector_stats=defaultdict(list); cards=[]
    for page in sorted({sid.split(':')[0] for sid in ids}):
        doc=read(base/'vector'/(page+'.json'))
        rows=read(snapshot/'pages'/(page+'.json'))
        for row in rows['subwords']:
            sid=f"{page}:l{row['line']}-w{row['word']}-s{row['subword']}"
            if sid not in ids:continue
            stub=sid.replace(':','-'); (assets/(stub+'-native.svg')).write_text(native_svg(row))
            split=split_lookup[page]
            if sid in metrics['trial']:
                rec=records[sid]
                with np.load(folder/rec['file']) as f: labels,roles=f['instances'],f['roles']
                ink=labels>0;join_ids=connector_ids(row,doc,roles) if rec['seenReh'] else []
                for name in ('previous','trial'):
                    with np.load(folder/'predictions'/name/(sid.replace(':','__')+'.npz')) as f:
                        pred,pr=f['instances'],f['roles']
                    if np.any((pred>0)!=ink):raise ValueError('Incomplete prediction coverage: '+sid)
                    draw(ink,pred,pr,assets/(stub+'-'+name+'.png'))
                    for j in join_ids:
                        connector_stats[(split,name)].append(connector_iou(labels,j,pred,pr))
                prev,trial=[metrics[k][sid]['shape'] for k in ('previous','trial')]
                old_frag=sum(l['bodyFragments']>=2 for l in prev['seen'])
                new_frag=sum(l['bodyFragments']>=2 for l in trial['seen'])
                gain=sum(l['iou'] for l in trial['seen'])-sum(l['iou'] for l in prev['seen'])
                flags=(['seen-reh'] if rec['seenReh'] else [])+(['tooth'] if rec['separateTooth'] else [])
                if old_frag>new_frag:flags.append('fixed')
                if new_frag:flags.append('split')
                if gain<-.1:flags.append('lower')
                if sid in {'page116:l8-w8-s1','page005:l11-w5-s2'}:flags.append('example')
                note=f"Seen body IoU {prev['seen'][0]['iou']:.0%} → {trial['seen'][0]['iou']:.0%}" if len(prev['seen'])==1 else ''
                refs[sid]=dict(oldBodies=prev['predictedBodies'],newBodies=trial['predictedBodies'],note=note,
                               flags=flags,gain=gain,oldFragments=old_frag,newFragments=new_frag)
            else:
                raster=doc['raster']
                runs=raster_runs(row['sourceInk'],raster['width'],raster['height'],doc['pdfToRaster'])
                ink,_=source_crop(dict(id=sid,sourceRuns=runs),raster['width'])
                example=canvas_example(dict(id=sid,group='display',pdfToRaster=doc['pdfToRaster']),
                                       ink.astype('uint16'),np.array([0,1],'uint8'))
                illustrations.append(example)
                refs[sid]=dict(flags=['open'] if sid in native['openSeenReh'] else ['example'],gain=0,
                    note='Partial native cuts · visual comparison only' if not row['complete'] else
                         'Native cuts complete · excluded from fitting and scoring by raster check')
            cards.append(dict(id=sid,stub=stub,text=row['text'],split=split,expected=len(row['baseText'])))
    if illustrations:
        torch.set_num_threads(4)
        settings=read(base/'mask2former/settings.json')
        for name,path in [('previous',base/'mask2former/best.pt'),('trial',folder/'best.pt')]:
            model=get_model(base,path,torch.device('cpu'))
            for e in illustrations:
                sid=e['record']['id'];ink=e['labels']>0
                p=predict(model,'mask2former',[e],torch.device('cpu'),settings['threshold'],min_region_area=settings['decoder'])[0]
                draw(ink,p['prediction'],p['roles'],assets/(sid.replace(':','-')+'-'+name+'.png'))
                save_prediction(folder/'illustrative-predictions'/name,sid,p)
                refs[sid]['oldBodies' if name=='previous' else 'newBodies']=int(sum(p['roles']==1))
            del model
    for row in cards:row.update(refs[row['id']])
    connectors={split:{name:dict(total=len(x),matches50=sum(v>=.5 for v in x),
                    meanIoU=sum(x)/max(1,len(x))) for (s,name),x in connector_stats.items() if s==split}
                    for split in ('test','validation')}
    write(folder/'connector-evaluation.json',connectors)
    write(output/'examples.json',cards)
    write(output/'scores.json',dict(heldOut=report,connectors=connectors,native=native))
    write(output/'training.json',dict(recipe=recipe,selected=read(folder/'training.json')))
    html(output,cards,report,connectors,native)
    preview(folder, records)


def html(output,cards,report,connectors,native):
    def score_rows():
        split='test'
        a,b=[report['targets'][k][split] for k in ('previous','trial')]
        ag,bg=[report['general'][k] for k in ('previous','trial')]
        c,d=[connectors[split][k] for k in ('previous','trial')]
        panel=report['countPanel'][split]
        rows=[
            ('Fragmented س/ش before ر',a['seenReh']['fragmented'],b['seenReh']['fragmented'],a['seenReh']['seenLetters']),
            ('س/ش before ر recovered by one body mask at IoU ≥ 0.75',a['seenReh']['intact75'],b['seenReh']['intact75'],a['seenReh']['seenLetters']),
            ('Neighboring tooth and س/ش both recovered at IoU ≥ 0.50',a['separateTooth']['bothRecovered'],b['separateTooth']['bothRecovered'],a['separateTooth']['pairs']),
            ('Genuine connector between س/ش and ر recovered at IoU ≥ 0.50',c['matches50'],d['matches50'],c['total']),
            ('Previously overcounted test subwords still overcounted',panel['previousOver'],panel['trialOver'],panel['words']),
            ('General body/kashida instance F1 at IoU ≥ 0.50',f"{ag['weak_test']['roleAwareInstanceF1AtIoU50']:.2%}",f"{bg['weak_test']['roleAwareInstanceF1AtIoU50']:.2%}",ag['weak_test']['examples']),
            ('Human-reviewed validation instance F1 at IoU ≥ 0.50',f"{ag['human_validation']['roleAwareInstanceF1AtIoU50']:.2%}",f"{bg['human_validation']['roleAwareInstanceF1AtIoU50']:.2%}",ag['human_validation']['examples'])]
        return ''.join(f'<tr><td>{escape(str(s))}</td><td>{a}</td><td>{b}</td><td>{n}</td></tr>' for s,a,b,n in rows)
    def card(row):
        sid=escape(row['id']);stub=row['stub'];flags=' '.join(row['flags'])
        def figure(name,caption):
            file=stub+'-'+name+('.svg' if name=='native' else '.png')
            return f'<figure><img src="assets/{file}" alt="{escape(caption)}" loading="lazy"><figcaption>{escape(caption)}</figcaption></figure>'
        return (f'<article id="{stub}" data-search="{sid} {escape(row["text"])}" data-split="{row["split"]}" data-flags="{flags}">'
                f'<header><div><a href="#{stub}">{sid}</a> · {row["split"]}</div><b lang="ar" dir="rtl">{escape(row["text"])}</b></header>'
                f'<div class="images">'+figure('native','Template cuts')+figure('previous',f"Previous · {row['oldBodies']} bodies")+
                figure('trial',f"Trial · {row['newBodies']} bodies")+'</div>'
                f'<p>{escape(row["note"])} · {row["expected"]} transcript bodies</p></article>')
    priority=lambda r:(0 if r['id']=='page069:l4-w7-s2' else 1 if r['split']=='test' and 'fixed' in r['flags']
                       else 2 if r['split']=='test' else 3,r['id'])
    content=''.join(card(r) for r in sorted(cards,key=priority))
    document='''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Seen and sheen · Mask2Former trial</title>
<style>*{box-sizing:border-box}body{margin:28px auto;padding:0 18px;max-width:1050px;color:#142b33;background:#f7f8f5;font:16px/1.45 system-ui,sans-serif}
h1{font-size:clamp(24px,4vw,38px);margin:.5em 0}h2{margin:1.6em 0 .5em}a{color:#145872}section{overflow-x:auto}table{border-collapse:collapse;width:100%;background:white}
td,th{padding:.5em .8em;text-align:right;white-space:nowrap;border-bottom:1px solid #d8deda}td:first-child,th:first-child{text-align:left;white-space:normal}
input,select{font:inherit;padding:6px;max-width:100%}label{display:inline-flex;gap:6px;align-items:center;margin:4px 8px 4px 0}
article{border:1px solid #cfd8d4;margin:12px 0;padding:12px 16px;background:#fff;scroll-margin-top:10px}article:target{outline:2px solid #167279}header,.images{display:flex;justify-content:space-between;gap:8px}
header{align-items:center}header b{font:28px serif}.images figure{flex:1;min-width:0;margin:0;text-align:center}.images img{display:block;width:100%;height:125px;object-fit:contain;image-rendering:pixelated;padding:6px}
figcaption,article p{font-size:.86rem;color:#304951}article p{margin-bottom:3px}.legend{display:inline-block;background:#e5b63d;width:.8em;height:.8em}small{color:#43565d}</style>
<h1>Seen and sheen · Mask2Former trial</h1><p>Both checkpoints receive the same isolated ink and predict letter bodies and kashidas. Gold shows a kashida. Each body mask has its own color.</p>'''
    document+=f'<p>The templates completed {native["seenReh"]["completeNow"]} of {native["seenRehTotal"]} seen–reh subwords, up from {native["previousComplete"]}. The remaining {len(native["openSeenReh"])} are included below for visual review.</p>'
    if 'previouslyFalseComplete' in native:
        document+=f'<p>Of {native["previouslyFalseComplete"]} older cuts that gave the reh tail to the wrong body, {native["correctedTailOwnership"]} now place both bodies. The others are marked partial.</p>'
        document+='<p>The first evaluation revealed incorrect automatic references. This repeat uses corrected native masks and the same page splits and training recipe. Test pages stayed out of fitting and checkpoint selection. <a href="../seen-reh-first/">First trial with the original references</a></p>'
    document+='<h2>Held-out pages</h2><section><table><tr><th>Measure</th><th>Previous</th><th>Trial</th><th>Cases</th></tr>'+score_rows()+'</table></section>'
    document+='<p><small>All rows use test pages except the six human-reviewed validation words. Template masks are automatic references. The previously overcounted panel checks body counts only. A fragmented seen has at least two predicted bodies each covering 10% or more of its ink, with most of each body inside the seen.</small></p>'
    document+='''<h2>Review the cuts</h2><div><label>Pages <select id="split"><option value="">All shown</option><option>test</option><option>validation</option><option>train</option></select></label>
<label>Show <select id="kind"><option value="">All examples</option><option value="fixed">Fewer seen fragments</option><option value="split">Still fragmented</option><option value="lower">Lower seen overlap</option><option value="tooth">Neighboring tooth</option><option value="open">Open template cases</option><option value="seen-reh">Seen–reh</option></select></label>
<label>Find <input id="q" type="search" placeholder="Page, subword or text"></label><span id="count"></span></div>'''
    document+='<p><a href="scores.json">Evaluation results (JSON)</a> · <a href="training.json">Training recipe and checkpoint (JSON)</a></p>'
    document+=content+'''<script>const list=[...document.querySelectorAll('article')],split=document.getElementById('split'),kind=document.getElementById('kind'),q=document.getElementById('q');
function filter(){let n=0;for(const e of list){e.hidden=Boolean((split.value&&e.dataset.split!==split.value)||(kind.value&&!e.dataset.flags.split(' ').includes(kind.value))||(q.value&&!e.dataset.search.includes(q.value.trim())));if(!e.hidden)n++}document.getElementById('count').textContent=n+' examples'}
for(const el of [split,kind,q])el.addEventListener('input',filter);filter();</script></html>'''
    (output/'index.html').write_text(document)


if __name__=='__main__':
    parser=ArgumentParser(description=__doc__)
    for name in ('baseline','folder','native-results'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    render(args.baseline.resolve(),args.folder.resolve(),args.native_results.resolve())
