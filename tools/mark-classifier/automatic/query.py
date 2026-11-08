"""Dependency-free, read-only search and source-mask rendering for the corpus."""
from __future__ import annotations
from collections import Counter,OrderedDict
from contextlib import closing
import html
import json
import math
import mmap
from pathlib import Path
import re
import sqlite3
import struct
import threading
import time
import zlib
from urllib.parse import quote, urlencode

from .common import mask_path,plain,unpack_runs
from .scope import load_scope

COLOURS={'subword-ink':'#317bc5','body':'#317bc5','kashida':'#de8329','dot':'#9958b7','mark':'#168c79','symbol':'#b25470','unknown':'#b99761','other':'#78877f'}


def value(params,key,default=''):
    item=params.get(key,[default]);return item[0] if isinstance(item,list) else item


def number(params,key,default=None,integer=False,lo=0,hi=100_000_000):
    raw=value(params,key,'')
    if raw=='':return default
    try:result=int(raw) if integer else float(raw)
    except (TypeError,ValueError):raise ValueError(f'Invalid {key}')
    if not math.isfinite(result) or not lo<=result<=hi:raise ValueError(f'{key} out of range')
    return result


def get_meta(db,key):
    row=db.execute('SELECT value FROM meta WHERE key=?',(key,)).fetchone()
    return json.loads(row[0]) if row else None


def network_outputs(item,inference):
    """Decode saved independent heads without importing the numerical runtime."""
    blobs=[item.pop('role_scores',None),item.pop('type_scores',None)]
    vocabulary=(inference or {}).get('networkOutputs')
    if not vocabulary or any(blob is None for blob in blobs):return None
    if vocabulary.get('storage')!='float32-le':raise ValueError('Unsupported saved model score format')
    result={}
    for key,blob in zip(('roles','types'),blobs):
        names=vocabulary[key]
        if len(blob)!=4*len(names):raise ValueError('Saved model scores and vocabulary differ; regenerate inference')
        values=struct.unpack('<'+'f'*len(names),blob)
        result[key]=sorted([{'label':name,'score':score} for name,score in zip(names,values)],key=lambda x:-x['score'])
    chosen_role=item.get('raw_prediction_kind');chosen_type=item.get('raw_prediction_type')
    result['selectedRoleScore']=next((v['score'] for v in result['roles'] if v['label']==chosen_role),None)
    result['selectedTypeScore']=next((v['score'] for v in result['types'] if v['label']==chosen_type),None)
    result['typeApplies']=chosen_role in ('dot','mark','symbol')
    result['decoder']=(inference or {}).get('decoder')
    return result


class Corpus:
    def __init__(self,root):
        self.root=Path(root);self.path=self.root/'corpus.sqlite';self.cache=None;self.cache_version=None;self.summaries=OrderedDict();self.images=OrderedDict();self.lock=threading.RLock();self.metadata_lock=threading.RLock();self.policy_cache=None
    def version(self):
        # Include WAL commits and atomic rebuilds so cached labels never survive
        # a changed corpus. Missing WAL is normal after a checkpoint.
        result=[]
        for path in (self.path,Path(str(self.path)+'-wal')):
            try:s=path.stat();result.extend((s.st_ino,s.st_size,s.st_mtime_ns))
            except FileNotFoundError:result.extend((0,0,0))
        return '-'.join(format(n,'x') for n in result)
    def connect(self):
        if not self.path.exists():raise ValueError('Automatic corpus is not built yet. Run automatic_pipeline.py first.')
        db=sqlite3.connect(f'file:{self.path}?mode=ro',uri=True,timeout=10);db.row_factory=sqlite3.Row;return closing(db)
    def status(self):
        path=self.root/'status.json'
        return json.loads(path.read_text()) if path.exists() else {'status':'not-started'}
    def query_options(self):
        version=self.version()
        with self.lock:
            if self.policy_cache and self.policy_cache[0]==version:return self.policy_cache[1]
        with self.connect() as db:options={'modelPolicy':bool(get_meta(db,'inference')),'indexes':{r[1] for r in db.execute('PRAGMA index_list(items)')}}
        with self.lock:self.policy_cache=(version,options)
        return options
    def metadata(self):
        # Statistics may take longer than thumbnails; they must not hold the
        # image/search cache lock while scanning the corpus.
        with self.metadata_lock:
            version=self.version()
            if self.cache is not None and self.cache_version==version:return self.cache
            with self.connect() as db:
                build=get_meta(db,'build');learning=get_meta(db,'learning');resolution=get_meta(db,'resolution');scope=get_meta(db,'scope');inference=get_meta(db,'inference')
                # The compact covering index avoids touching source-mask blobs
                # or sorting the full corpus for statistics. Older snapshots
                # can fall back to a single sequential pass without migration.
                indexes={r[1] for r in db.execute('PRAGMA index_list(items)')}
                scan='INDEXED BY item_label_summary' if 'item_label_summary' in indexes else 'NOT INDEXED'
                combinations=db.execute(f"""SELECT teacher_kind,teacher_type,prediction_kind,prediction_type,final_kind,final_type,
                    COUNT(*) count,SUM(model_agrees=0) conflicts,SUM(refined=1) refined,
                    SUM(final_kind='subword-ink' AND subword_id IS NULL) unmapped
                    FROM items {scan} WHERE in_scope=1
                    GROUP BY teacher_kind,teacher_type,prediction_kind,prediction_type,final_kind,final_type""").fetchall()
                source_counts={source:Counter() for source in ('teacher','model','final')};kind_counts=Counter();differences=Counter()
                conflicts=refined=unmapped=0
                for r in combinations:
                    n=r['count'];conflicts+=r['conflicts'] or 0;refined+=r['refined'] or 0;unmapped+=r['unmapped'] or 0
                    pairs=[(r[k],r[t]) for k,t in [('teacher_kind','teacher_type'),('prediction_kind','prediction_type'),('final_kind','final_type')]]
                    for source,pair in zip(source_counts,pairs):source_counts[source][pair]+=n
                    kind_counts[r['final_kind']]+=n
                    if len(set(pairs))>1:
                        for type in {r['teacher_type'],r['prediction_type'],r['final_type']}-{None,'unknown'}:differences[type]+=n
                source_types={source:[dict(kind=kind,type=type,count=n) for (kind,type),n in sorted(counts.items(),key=lambda x:-x[1])]
                    for source,counts in source_counts.items()}
                kinds=[dict(kind=kind,count=n) for kind,n in kind_counts.items()];types=source_types['final']
                subwords=db.execute('SELECT COUNT(*) FROM subwords WHERE in_scope=1').fetchone()[0];words=db.execute('SELECT COUNT(*) FROM words WHERE in_scope=1').fetchone()[0]
                totals=dict(db.execute('SELECT COUNT(*) pages,SUM(COALESCE(scope_ink_pixels,ink_pixels)) ink,SUM(COALESCE(scope_unknown_pixels,unknown_pixels)) unresolvedInk,SUM(component_count) components FROM pages').fetchone())
                totals['regions']=sum(kind_counts.values())
                splits=[dict(r) for r in db.execute('SELECT split,COUNT(*) pages FROM pages GROUP BY split')]
                reasons=[dict(r) for r in db.execute("SELECT unresolved_reason reason,COUNT(*) count,SUM(area) pixels FROM items INDEXED BY item_final_browse WHERE in_scope=1 AND final_kind='unknown' GROUP BY unresolved_reason")]
            self.cache={'build':build,'learning':learning,'resolution':resolution,'inference':inference,'unresolvedReasons':reasons,'unmappedBodies':unmapped,'kinds':kinds,'types':types,'subwords':subwords,'words':words,
                'totals':totals,'disagreements':conflicts,'refined':refined,'splits':splits,'status':self.status(),
                'typesBySource':source_types,'typeDifferences':dict(differences),'scope':scope}
            self.cache_version=version;return self.cache
    def search(self,params):
        kind=value(params,'kind','dot');source=value(params,'source','final');mode=value(params,'quality','all')
        if kind not in ['all','subwords','unmapped-bodies','dot','mark','symbol','unknown','other','subword-ink','body','kashida']:raise ValueError('Invalid ink kind')
        if source not in ('final','teacher','model'):raise ValueError('Invalid label source')
        if mode not in ('all','supported','flagged','disagreement','refined','human','recovered','unmapped'):raise ValueError('Invalid quality filter')
        options=self.query_options();model_policy=source!='teacher' and options['modelPolicy']
        label_column={'final':'final_kind','teacher':'teacher_kind','model':'prediction_kind'}[source]
        type_column={'final':'final_type','teacher':'teacher_type','model':'prediction_type'}[source]
        metrics={'width':'w','height':'h','area':'area','components':'components','confidence':'model_confidence','quality':'quality'}
        metric=value(params,'metric','width')
        if metric not in metrics:raise ValueError('Invalid metric')
        order=value(params,'order','desc')
        if order not in ('asc','desc'):raise ValueError('Invalid sort order')
        # Large UI pages are loaded in bounded batches, including the All view.
        limit=number(params,'limit',1000,True,1,10000);offset=number(params,'offset',0,True,0,2_000_000)
        args=[];is_sub=kind=='subwords';alias='s' if is_sub else 'i';clauses=[f'{alias}.in_scope=1']
        query=value(params,'q','').strip()
        if len(query)>256:raise ValueError('Search text too long')
        if query:
            escaped=plain(query).replace('\\','\\\\').replace('%','\\%').replace('_','\\_')
            clauses.append(f"({alias}.bases LIKE ? ESCAPE '\\' OR w.bases LIKE ? ESCAPE '\\')");args.extend(['%'+escaped+'%']*2)
        for key,op in [('pageMin','>='),('pageMax','<=')]:
            num=number(params,key,None,True,1,604)
            if num is not None:clauses.append(f'{alias}.page_id {op} ?');args.append(f'page{num:03d}')
        line=number(params,'line',None,True,1,100)
        if line is not None:clauses.append(f'{alias}.line=?');args.append(line)
        split=value(params,'split','all')
        if split!='all':
            if split not in ('train','validation','test'):raise ValueError('Invalid split')
            clauses.append('p.split=?');args.append(split)
        if not is_sub:
            if kind=='unmapped-bodies':clauses.append("i.final_kind='subword-ink' AND i.subword_id IS NULL")
            elif kind!='all':clauses.append(f'i.{label_column}=?');args.append(kind)
            subtype=value(params,'type','all')
            if subtype!='all':
                if len(subtype)>64:raise ValueError('Invalid type')
                clauses.append(f'i.{type_column}=?');args.append(subtype)
            if mode=='supported':
                clauses.append("((i.human_reviewed=1 AND i.origin='human-reviewed') OR (i.model_confidence>=0.90 AND i.prediction_guard IS NULL))" if model_policy and source=='final' else
                    "(i.model_confidence>=0.90 AND i.prediction_guard IS NULL)" if model_policy else
                    "((i.quality>=0.65 AND (i.model_agrees=1 OR i.model_agrees IS NULL)) OR i.resolution_method IS NOT NULL)")
            elif mode=='flagged':clauses.append("(i.model_confidence<0.90 OR i.prediction_guard IS NOT NULL OR i.final_kind='unknown')" if model_policy else '(i.quality<0.5 OR i.model_agrees=0 OR i.final_kind=\'unknown\')')
            elif mode=='disagreement':clauses.append('i.model_agrees=0')
            elif mode=='refined':clauses.append('i.refined=1')
            elif mode=='human':clauses.append('i.human_reviewed=1')
            elif mode=='recovered':clauses.append('i.resolution_method IS NOT NULL')
            elif mode=='unmapped':clauses.append("i.assignment_status IN ('text-unassigned','unassigned')")
            reason=value(params,'reason','all')
            if reason!='all':
                if reason not in ('legacy-omission','ambiguous-geometry','incomplete-geometry','proposal-mismatch','inventory-unassigned','human-draft'):raise ValueError('Invalid unresolved reason')
                clauses.append('i.unresolved_reason=?');args.append(reason)
            metric_sql='i.'+metrics[metric]
            if metric=='confidence':clauses.append('i.model_confidence IS NOT NULL')
        else:
            if mode=='supported':clauses.append("s.issues='[]'")
            elif mode=='flagged':clauses.append("s.issues<>'[]'")
            elif mode in ('disagreement','refined','human'):
                test={'disagreement':'model_agrees=0','refined':'refined=1','human':'human_reviewed=1'}[mode]
                clauses.append(f'EXISTS(SELECT 1 FROM items a INDEXED BY item_subword WHERE a.subword_id=s.id AND a.{test})')
            metric_sql='s.'+('h' if metric=='height' else 'w')
        for key,op in [('min','>='),('max','<=')]:
            num=number(params,key)
            if num is not None:clauses.append(f'{metric_sql}{op}?');args.append(num)
        where=' AND '.join(clauses) if clauses else '1'
        if is_sub:
            tables='subwords s JOIN pages p ON p.page_id=s.page_id LEFT JOIN words w ON w.id=s.word_id'
            fields="s.*,w.text word_text,p.split,'subword-ink' final_kind,'subword-body' final_type,'subword' entity,NULL model_confidence,NULL prediction_kind,NULL prediction_type,NULL model_agrees,NULL origin,0 human_reviewed"
        else:
            tables='items i JOIN pages p ON p.page_id=i.page_id LEFT JOIN words w ON w.id=i.word_id'
            fields=f'i.id,i.page_id,i.word_id,i.subword_id,i.line,i.text,i.x,i.y,i.w,i.h,i.area,i.components,i.quality,i.origin,i.human_reviewed,i.teacher_kind,i.teacher_type,i.prediction_kind,i.prediction_type,i.model_confidence,i.model_margin,i.model_agrees,i.raw_prediction_kind,i.raw_prediction_type,i.raw_model_confidence,i.prediction_guard,i.final_kind,i.final_type,i.refined,i.issues,i.unresolved_reason,i.resolution_method,i.resolution_score,i.assignment_status,w.text word_text,p.split,i.{label_column} display_kind,i.{type_column} display_type,\'item\' entity'
        # A width-ordered index scans hundreds of thousands of larger bodies
        # before reaching small dots. Narrow by label first, even for LIMIT/OFFSET.
        # Keep page/text joins only when a filter actually needs them.
        if is_sub:aggregate_tables='subwords s'
        else:
            constrained=kind!='all' or subtype!='all'
            index=f'item_{"final" if kind=="unmapped-bodies" else source}_browse' if constrained else 'item_width_browse'
            supported_index=f'item_{source}_supported_browse'
            if model_policy and mode=='supported' and supported_index in options['indexes']:index=supported_index
            if model_policy and mode=='flagged' and 'item_attention_score' in options['indexes']:index='item_attention_score'
            aggregate_tables=f'items i INDEXED BY {index}'
        if split!='all':aggregate_tables+=f' JOIN pages p ON p.page_id={alias}.page_id'
        if query:aggregate_tables+=f' LEFT JOIN words w ON w.id={alias}.word_id' 
        version=self.version();key=(version,aggregate_tables,where,tuple(args),metric_sql)
        with self.lock:
            summary=self.summaries.get(key)
            if summary is not None:self.summaries.move_to_end(key)
        with self.connect() as db:
            if summary is None:
                total,low,high=db.execute(f'SELECT COUNT(*),MIN({metric_sql}),MAX({metric_sql}) FROM {aggregate_tables} WHERE {where}',args).fetchone()
                histogram=[]
                if total and low is not None:
                    span=max(1e-9,high-low)
                    bins=[dict(r) for r in db.execute(f'SELECT MIN(15,CAST(({metric_sql}-?)/?*16 AS INTEGER)) bin,COUNT(*) count FROM {aggregate_tables} WHERE {where} GROUP BY bin',[low,span]+args)]
                    histogram={'min':low,'max':high,'bins':bins}
                summary=(total,histogram)
                with self.lock:
                    self.summaries[key]=summary
                    while len(self.summaries)>32:self.summaries.popitem(last=False)
            total,histogram=summary
            # Page over narrow IDs first. Fetch transcript and full card columns
            # only for that batch, rather than joining every skipped region.
            rows=[dict(r) for r in db.execute(f'WITH selected AS (SELECT {alias}.id FROM {aggregate_tables} WHERE {where} ORDER BY {metric_sql} {order.upper()},{alias}.id LIMIT ? OFFSET ?) SELECT {fields} FROM {tables} JOIN selected chosen ON chosen.id={alias}.id ORDER BY {metric_sql} {order.upper()},{alias}.id',args+[limit,offset])]
        for row in rows:
            row['thumbnail']=f"/api/automatic/image.svg?id={quote(row['id'],safe='')}&entity={row['entity']}&source={source}&v={version}"
        return {'total':total,'offset':offset,'limit':limit,'items':rows,'histogram':histogram,'metric':metric,'source':source}
    def detail(self,id,entity='item'):
        if len(id)>256:raise ValueError('Invalid instance ID')
        with self.connect() as db:
            if entity=='subword':
                row=db.execute('SELECT * FROM subwords WHERE id=? AND in_scope=1',(id,)).fetchone()
                if not row:raise ValueError('Subword not found')
                item=dict(row);selected=[dict(r) for r in db.execute("SELECT * FROM items INDEXED BY item_subword WHERE subword_id=? AND in_scope=1 AND final_kind IN ('subword-ink','body','kashida')",(id,))]
                wid=item['word_id'];item.update(entity='subword',final_kind='subword-ink',final_type='subword-body')
                item['area']=sum(r['area'] for r in selected)
                item['components']=sum(r['components'] for r in selected)
            elif entity=='item':
                row=db.execute('SELECT * FROM items WHERE id=? AND in_scope=1',(id,)).fetchone()
                # A regrouped source component remains addressable through its
                # original ID; its ink now belongs to one complete mark.
                if not row and re.fullmatch(r'page\d{3}:source:part-\d+',id):
                    page_id,_,part_id=id.split(':')
                    cache=self.root/'pages'/f'{page_id}.json.zlib'
                    if cache.exists():
                        payload=json.loads(zlib.decompress(cache.read_bytes()))
                        parents=[i['id'] for i in payload['items'] if part_id in i.get('region_ids',[])]
                        if len(parents)==1:
                            row=db.execute('SELECT * FROM items WHERE id=? AND in_scope=1',(parents[0],)).fetchone()
                if not row:raise ValueError('Instance not found')
                item=dict(row);selected=[item];wid=item['word_id'];item['entity']='item'
                if row['id']!=id:item['requestedComponentId']=id
            else:raise ValueError('Invalid entity')
            page=dict(db.execute('SELECT * FROM pages WHERE page_id=?',(item['page_id'],)).fetchone())
            word=db.execute('SELECT * FROM words WHERE id=?',(wid,)).fetchone() if wid else None
            siblings=[dict(r) for r in db.execute('SELECT id,final_kind,final_type,x,y,w,h FROM items WHERE word_id=? AND in_scope=1',(wid,))] if wid else []
            item['networkOutputs']=network_outputs(item,get_meta(db,'inference'))
        masks=[]
        for region in selected:
            masks.append({'runs':unpack_runs(region['runs']),'kind':region['final_kind'],'bbox':[region['x'],region['y'],region['w'],region['h']]})
        item.pop('runs',None)
        item['source_diagnostics']=json.loads(item.get('source_diagnostics') or '{}')
        item['prediction_structure']=json.loads(item.get('prediction_structure') or '{}')
        item['review_attribution']=json.loads(item.get('review_attribution') or 'null')
        item['context_bbox']=json.loads(item['context_bbox']) if item.get('context_bbox') else None
        item['issues']=json.loads(item.get('issues','[]'));item['masks']=masks;item['page']=page;item['word']=dict(word) if word else None;item['siblings']=siblings
        item['maskPath']=''.join(mask_path(mask['runs'],page['width']) for mask in masks)
        prepared=self.root/'training-labels'/'labels.sqlite'
        if prepared.exists():
            with closing(sqlite3.connect(f'file:{prepared}?mode=ro',uri=True)) as labels:
                if labels.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='corrections'").fetchone():
                    correction=labels.execute('SELECT evidence FROM corrections WHERE item_id=?',(item['id'],)).fetchone()
                    if correction:item['trainingLabelCorrection']=json.loads(correction[0])
        scope=load_scope(self.root,item['page_id'],page['source_sha'])
        item['scopedSourceUrl']=f"/api/pages/{item['page_id']}/source.svg?scope=interior" if scope else None
        raw_id=item['id'].split(':',1)[1]
        automatic=None;proposal_file=self.root/'proposals'/f"{item['page_id']}.json"
        if proposal_file.exists():
            from review_bridge import proposal_digest
            snapshot=json.loads(proposal_file.read_text())
            if proposal_digest(snapshot)==page.get('proposal_sha'):automatic=snapshot
        query={'proposalSource':'automatic' if automatic else 'legacy'};item['reviewCanSuggest']=False
        if entity=='subword':
            if not automatic or any(s['id']==raw_id for s in automatic['subwords']):query.update(queue='subwords',candidate=raw_id)
        elif automatic and any(c['id']==raw_id for c in automatic['candidates']):
            query['candidate']=raw_id;item['reviewCanSuggest']=True
        elif item.get('review_candidate'):
            query={'proposalSource':'legacy','candidate':item['review_candidate']};item['reviewCanSuggest']=True
        if item['reviewCanSuggest'] and item['final_kind'] in ('dot','mark','symbol','other'):
            query.update(suggestRole=item['final_kind'],suggestType=item['final_type'])
        item['reviewUrl']='/review.html'+('?' + urlencode(query) if query else '')+'#'+item['page_id']
        item['workbenchUrl']=f"/index.html#{item['page_id']}"
        return item
    def image(self,id,entity='item',source='final'):
        if len(id)>256 or entity not in ('item','subword'):raise ValueError('Invalid instance')
        column={'final':'final_kind','teacher':'teacher_kind','model':'prediction_kind'}.get(source)
        if not column:raise ValueError('Invalid label source')
        key=(self.version(),id,entity,source)
        with self.lock:
            if key in self.images:self.images.move_to_end(key);return self.images[key]
        # Thumbnails need only masks and raster width, not transcript, sibling
        # queries, diagnostic JSON, or the full source-context detail payload.
        with self.connect() as db:
            if entity=='item':
                rows=db.execute(f'SELECT i.runs,i.x,i.y,i.w,i.h,i.{column} kind,p.width FROM items i JOIN pages p ON p.page_id=i.page_id WHERE i.id=? AND i.in_scope=1',(id,)).fetchall()
                if not rows:raise ValueError('Instance not found')
            else:
                if not db.execute('SELECT 1 FROM subwords WHERE id=?',(id,)).fetchone():raise ValueError('Subword not found')
                rows=db.execute("SELECT i.runs,i.x,i.y,i.w,i.h,i.final_kind kind,p.width FROM items i INDEXED BY item_subword JOIN pages p ON p.page_id=i.page_id WHERE i.subword_id=? AND i.in_scope=1 AND i.final_kind IN ('subword-ink','body','kashida')",(id,)).fetchall()
        boxes=[[r[k] for k in ('x','y','w','h')] for r in rows]
        if not boxes:return '<svg xmlns="http://www.w3.org/2000/svg" width="180" height="110"><text x="15" y="55" fill="#8b8c79" font-size="12">Unresolved body</text></svg>'
        x=min(b[0] for b in boxes);y=min(b[1] for b in boxes);w=max(b[0]+b[2] for b in boxes)-x;h=max(b[1]+b[3] for b in boxes)-y
        pad=max(3,min(20,max(w,h)*.08));colour=COLOURS.get(rows[0]['kind'],'#b99761')
        path=''.join(mask_path(unpack_runs(r['runs']),r['width']) for r in rows)
        result=f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x-pad} {y-pad} {w+2*pad} {h+2*pad}" width="180" height="110"><path fill="{colour}" d="{path}"/></svg>'
        with self.lock:
            self.images[key]=result
            while len(self.images)>512:self.images.popitem(last=False)
        return result
    def similar(self,id,limit=24):
        path=self.root/'embeddings.f16'
        if not path.exists():raise ValueError('Learned embeddings are not available yet')
        with self.connect() as db:
            if not get_meta(db,'learning'):raise ValueError('The new corpus is still being trained; similarity will be available when it finishes.')
            target=db.execute('SELECT feature_row,final_kind,final_type,w,h FROM items WHERE id=? AND in_scope=1',(id,)).fetchone()
            if not target:raise ValueError('Instance not found')
            pool=db.execute('SELECT id,feature_row FROM items WHERE in_scope=1 AND final_kind=? AND id<>? ORDER BY ABS(w*1.0/MAX(1,h)-?) LIMIT 6000',
                (target['final_kind'],id,target['w']/max(1,target['h']))).fetchall()
        with path.open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as mm:
            def embedding(row):return struct.unpack_from('<64e',mm,row*128)
            reference=embedding(target['feature_row']);ranked=[]
            for row in pool:
                v=embedding(row['feature_row']);ranked.append((sum(a*b for a,b in zip(reference,v)),row['id']))
        ranked.sort(reverse=True);ids=[i for _,i in ranked[:min(60,limit)]]
        with self.connect() as db:
            found={r['id']:dict(r) for r in db.execute(f"SELECT id,page_id,text,final_kind,final_type,model_confidence,w,h,area,refined FROM items WHERE id IN ({','.join('?' for _ in ids)})",ids)} if ids else {}
        rows=[]
        for similarity,i in ranked[:min(60,limit)]:
            row=found[i];row.update(entity='item',similarity=similarity,thumbnail=f'/api/automatic/image.svg?id={quote(i,safe="")}');rows.append(row)
        return {'total':len(rows),'items':rows,'reference':id,'method':'Nearest learned source-mask/context/geometry embeddings within the same corpus ink role'}
