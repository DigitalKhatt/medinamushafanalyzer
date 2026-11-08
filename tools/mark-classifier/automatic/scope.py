"""Exact SVG frame interiors, shared by automatic and manual views.

The source SVG and original ink masks are immutable. A scope is an explicit,
hashed inclusion boundary; excluded source ink is retained for audit.
"""
from __future__ import annotations
from bisect import bisect_right
from concurrent.futures import ProcessPoolExecutor
import copy
from collections import Counter
import hashlib
import json
from pathlib import Path
import sqlite3
import xml.etree.ElementTree as ET

from .common import unpack_runs
from .text_scope import basmala_lines, text_exclusions

SVG='http://www.w3.org/2000/svg';NS='{'+SVG+'}';LEGACY_VERSION='svg-frame-interior-v1';TITLE_VERSION='svg-frame-interior-v2-no-surah-titles';VERSION='svg-frame-interior-v3-text-corpus'


def assets_folder(root,version=VERSION):
    if version not in (LEGACY_VERSION,TITLE_VERSION,VERSION):raise ValueError('Unsupported frame scope version')
    return Path(root)/'scope' if version==LEGACY_VERSION else Path(root)/'scope'/version


def frame_geometry(svg_bytes, *, page_frame_only=False):
    """Resolve inherited transforms and pick the unique page-sized green path.
    Keep the Quran text interiors and subtract surah title frames and their text.
    Use page_frame_only to keep every opening of the page frame.
    """
    import numpy as np
    from svgpathtools import parse_path
    from svgpathtools.parser import parse_transform
    from svgpathtools.path import transform
    root=ET.fromstring(svg_bytes);view_box=[float(v) for v in root.get('viewBox').replace(',',' ').split()]
    candidates=[];green_paths=[]
    def walk(element,matrix,style):
        if element.tag in (NS+'defs',NS+'clipPath'):return
        matrix=matrix@parse_transform(element.get('transform',''))
        style={**style,**{k:v for k,v in element.attrib.items() if k in ('fill','fill-rule')},
            **dict(pair.split(':',1) for pair in element.get('style','').split(';') if ':' in pair)}
        fill=style.get('fill','').strip().lower()
        if element.tag==NS+'path' and fill.startswith('#') and len(fill)==7:
            r,g,b=(int(fill[i:i+2],16) for i in (1,3,5))
            if g-r>12 and g-b>8:
                path=transform(parse_path(element.get('d','')),matrix);x0,x1,y0,y1=path.bbox()
                green_paths.append((element,path,style.get('fill-rule','nonzero')))
                if x1-x0>view_box[2]*.45 and y1-y0>view_box[3]*.45:
                    candidates.append((element,path,style.get('fill-rule','nonzero')))
        for child in element:walk(child,matrix,style)
    walk(root,np.eye(3),{})
    if len(candidates)!=1:raise ValueError(f'Expected one page frame path, found {len(candidates)}')
    element,frame,fill_rule=candidates[0]
    if fill_rule!='evenodd':raise ValueError('Unexpected frame fill rule')
    contours=frame.continuous_subpaths()
    def box_area(path):
        a,b,c,d=path.bbox();return (b-a)*(d-c)
    outer=max(contours,key=box_area);holes=[p for p in contours if p is not outer]
    # SVG fill closes open subpaths implicitly; preserve that straight closing
    # edge with Z rather than rejecting valid source artwork.
    if not holes:raise ValueError('Frame has no interior')
    a,b,c,d=outer.bbox()
    for hole in holes:
        x0,x1,y0,y1=hole.bbox()
        if x0<a-1e-5 or x1>b+1e-5 or y0<c-1e-5 or y1>d+1e-5:raise ValueError('Frame contours are not nested')
    if page_frame_only:
        boxes = [[x0,y0,x1-x0,y1-y0] for x0,x1,y0,y1 in (p.bbox() for p in holes)]
        return {'framePathId':element.get('id'),'viewBox':view_box,
                'interiorPath':' '.join(p.d()+' Z' for p in holes),'interiorBBoxes':boxes}
    kept=list(holes);title_frames=[]
    if len(holes)>1:
        # The opening pages embed their title panel in the page-frame path.
        # Their largest opening is the Quran text; smaller openings are titles.
        main=max(holes,key=box_area);kept=[main]
        for hole in holes:
            if hole is not main:
                x0,x1,y0,y1=hole.bbox()
                title_frames.append({'pathId':element.get('id'),'source':'page-frame-inner-contour',
                    'outerPath':hole.d()+' Z','bbox':[x0,y0,x1-x0,y1-y0]})
    exclusions=[];verse_markers=[]
    for title_element,path,rule in green_paths:
        if title_element is element:continue
        x0,x1,y0,y1=path.bbox();w,h=x1-x0,y1-y0
        # Surah frames are wide, shallow compound paths. Verse markers and
        # margin ornaments have distinct proportions and stay untouched.
        if (.015*view_box[2]<w<.06*view_box[2] and 1.15*w<h<1.6*w
                and any(x0>=p.bbox()[0] and x1<=p.bbox()[1] and y0>=p.bbox()[2] and y1<=p.bbox()[3] for p in kept)):
            verse_markers.append({'pathId':title_element.get('id'),'bbox':[x0,y0,w,h]})
        if not (w>view_box[2]*.4 and 0<h<view_box[3]*.15 and w/h>3):continue
        subs=path.continuous_subpaths()
        if rule!='evenodd' or len(subs)!=2:raise ValueError('Unexpected surah frame contours')
        title_outer=max(subs,key=box_area)
        if not any(x0>=p.bbox()[0]-1e-5 and x1<=p.bbox()[1]+1e-5 and y0>=p.bbox()[2]-1e-5 and y1<=p.bbox()[3]+1e-5 for p in kept):
            raise ValueError('Surah frame lies outside the Quran text opening')
        exclusions.append(title_outer)
        title_frames.append({'pathId':title_element.get('id'),'source':'surah-frame-path',
            'outerPath':title_outer.d()+' Z','bbox':[x0,y0,w,h]})
    # Even-odd clipping subtracts the complete banner footprint, including
    # its white title opening, while leaving the following bismillah intact.
    interior=' '.join(p.d()+' Z' for p in kept+exclusions)
    return {'version':VERSION,'sourceSha256':hashlib.sha256(svg_bytes).hexdigest(),'framePathId':element.get('id'),
        'viewBox':view_box,'interiorPath':interior,'frameBBox':[a,c,b-a,d-c],
        'interiorContours':len(holes),'includedContours':len(kept),'surahFrames':title_frames,'verseMarkers':verse_markers,
        'interiorBBoxes':[[p.bbox()[0],p.bbox()[2],p.bbox()[1]-p.bbox()[0],p.bbox()[3]-p.bbox()[2]] for p in kept]}


def clean_svg(svg_bytes,geometry):
    root=ET.fromstring(svg_bytes);root=copy.deepcopy(root)
    matches=[];remove_ids={geometry['framePathId'],*(s['pathId'] for s in geometry.get('surahFrames',[]))}
    for parent in root.iter():
        for child in parent:
            if child.get('id') in remove_ids:matches.append((parent,child))
    if len(matches)!=len(remove_ids):raise ValueError('Frame path identity is not unique')
    for parent,child in matches:parent.remove(child)
    clip_id='corpus-frame-interior'
    if any(e.get('id')==clip_id for e in root.iter()):raise ValueError('Scope clip ID collision')
    defs=ET.SubElement(root,NS+'defs');clip=ET.SubElement(defs,NS+'clipPath',{'id':clip_id,'clipPathUnits':'userSpaceOnUse'})
    ET.SubElement(clip,NS+'path',{'d':geometry['interiorPath'],'clip-rule':'evenodd'})
    group=ET.Element(NS+'g',{'clip-path':f'url(#{clip_id})'})
    for child in list(root):
        if child.tag!=NS+'defs':root.remove(child);group.append(child)
    root.append(group)
    return ET.tostring(root,encoding='utf-8',xml_declaration=True)


def raster_scope(geometry,width,height):
    import numpy as np
    from PySide6.QtCore import QByteArray
    from PySide6.QtGui import QImage,QPainter
    from PySide6.QtSvg import QSvgRenderer
    view=' '.join(str(v) for v in geometry['viewBox'])
    svg=f'<svg xmlns="{SVG}" viewBox="{view}" preserveAspectRatio="none"><path fill="white" fill-rule="evenodd" d="{geometry["interiorPath"]}"/></svg>'
    renderer=QSvgRenderer(QByteArray(svg.encode()))
    if not renderer.isValid():raise ValueError('Invalid vector scope')
    image=QImage(width,height,QImage.Format.Format_RGB32);image.fill(0)
    painter=QPainter(image);renderer.render(painter);painter.end()
    return (np.frombuffer(image.constBits(),dtype='u1').reshape(height,image.bytesPerLine())[:,:width*4:4]>=128).copy()


def mask_runs(mask):
    import numpy as np
    flat=mask.ravel();changes=np.flatnonzero(np.diff(np.r_[False,flat,False].astype('i1')))
    return [int(v) for pair in zip(changes[::2],changes[1::2]-changes[::2]) for v in pair]


def clip_runs(runs,allowed,starts=None):
    """Intersect immutable source spans with a scope; never add source pixels."""
    starts=starts if starts is not None else allowed[::2];out=[]
    for s,n in zip(runs[::2],runs[1::2]):
        end=s+n;i=max(0,bisect_right(starts,s)-1)
        while i<len(starts) and starts[i]<end:
            left=max(s,starts[i]);right=min(end,starts[i]+allowed[2*i+1])
            if right>left:
                if out and out[-2]+out[-1]==left:out[-1]+=right-left
                else:out.extend((left,right-left))
            i+=1
    return out


def ensure_assets(root,svg_bytes,page_id,width,height,source_sha):
    root=Path(root)
    if hashlib.sha256(svg_bytes).hexdigest()!=source_sha:raise ValueError(f'{page_id}: source SVG changed')
    folder=assets_folder(root);folder.mkdir(parents=True,exist_ok=True);target=folder/f'{page_id}.json'
    old=json.loads(target.read_text()) if target.exists() else {}
    if (folder/f'{page_id}.svg').exists() and old.get('version')==VERSION and old.get('sourceSha256')==source_sha and old.get('width')==width and old.get('height')==height:
        geometry=old;allowed=geometry['runs']
    else:
        geometry=frame_geometry(svg_bytes);mask=raster_scope(geometry,width,height);allowed=mask_runs(mask)
        geometry.update(pageId=page_id,width=width,height=height,runs=allowed)
        geometry['scopeSha256']=hashlib.sha256(json.dumps(geometry,sort_keys=True,separators=(',',':')).encode()).hexdigest()
        svg_target=folder/f'{page_id}.svg';svg_temp=svg_target.with_suffix('.svg.tmp');svg_temp.write_bytes(clean_svg(svg_bytes,geometry));svg_temp.replace(svg_target)
        temp=target.with_suffix('.json.tmp');temp.write_text(json.dumps(geometry,separators=(',',':')));temp.replace(target)
    return geometry


def _prepare_page(task):
    root,source,page,openings=task;root,source=Path(root),Path(source)
    page_id=page['page_id'];svg_bytes=(source/f'{page_id}.svg').read_bytes()
    geometry=ensure_assets(root,svg_bytes,page_id,page['width'],page['height'],page['source_sha']);allowed=geometry['runs']
    db=sqlite3.connect(f'file:{root/"corpus.sqlite"}?mode=ro',uri=True);db.row_factory=sqlite3.Row
    rows=db.execute('SELECT id,runs,area,human_reviewed,word_id,x,y,w,h,teacher_type FROM items WHERE page_id=?',(page_id,)).fetchall()
    words=db.execute('SELECT id,line,bases,x,y,w,h FROM words WHERE page_id=?',(page_id,)).fetchall();db.close()
    text=text_exclusions(geometry,rows,words,openings)
    excluded=[];partial=[];inside=0;outside=0;starts=allowed[::2]
    for row in rows:
        runs=unpack_runs(row['runs']);kept=[] if row['id'] in text else clip_runs(runs,allowed,starts);area=sum(kept[1::2]);inside+=area;outside+=row['area']-area
        if not area:excluded.append(row['id'])
        elif area!=row['area']:partial.append({'id':row['id'],'runs':kept,'area':area,'sourceArea':row['area'],'humanReviewed':row['human_reviewed']})
    return {'pageId':page_id,'framePathId':geometry['framePathId'],'interiorContours':geometry['interiorContours'],
        'surahFrames':geometry.get('surahFrames',[]),'insidePixels':inside,'outsidePixels':outside,'excludedIds':excluded,'partial':partial,'textExclusions':text,'verseMarkers':len(geometry['verseMarkers']),
        'basmalaOpenings':len(openings)}


def prepare_scopes(data_root,root,jobs,progress):
    root=Path(root);source=Path(json.loads((Path(data_root)/'corpus.json').read_text())['sourceDir'])
    (root/'scope').mkdir(parents=True,exist_ok=True)
    db=sqlite3.connect(root/'corpus.sqlite');db.row_factory=sqlite3.Row
    pages=[dict(r) for r in db.execute('SELECT * FROM pages ORDER BY page_id')];db.close()
    openings=basmala_lines()
    results=[]
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        for n,result in enumerate(pool.map(_prepare_page,((str(root),str(source),p,openings.get(p['page_id'],())) for p in pages)),1):
            results.append(result);progress('scope',n,len(pages),{'pageId':result['pageId'],'excluded':len(result['excludedIds']),'partial':len(result['partial'])})
    return results


def apply_scopes(data_root,root,jobs,progress,prepared=None):
    root=Path(root);results=prepared if prepared is not None else prepare_scopes(data_root,root,jobs,progress)
    partial=[(r['pageId'],p['id']) for r in results for p in r['partial']]
    if partial:raise ValueError(f'Source groups cross the frame boundary; split them before publishing scope: {partial[:8]}')
    db=sqlite3.connect(root/'corpus.sqlite')
    for table in ('items','words','subwords'):
        if 'in_scope' not in {r[1] for r in db.execute(f'PRAGMA table_info({table})')}:
            db.execute(f'ALTER TABLE {table} ADD COLUMN in_scope INTEGER NOT NULL DEFAULT 1')
    for name in ('scope_ink_pixels','scope_unknown_pixels'):
        if name not in {r[1] for r in db.execute('PRAGMA table_info(pages)')}:db.execute(f'ALTER TABLE pages ADD COLUMN {name} INTEGER')
    db.execute('UPDATE items SET in_scope=1 WHERE in_scope<>1')
    db.executemany('UPDATE items SET in_scope=0 WHERE id=?',((id,) for r in results for id in r['excludedIds']))
    # Retain missing-geometry logical words; scope never invents their masks.
    db.execute('''UPDATE words SET in_scope=CASE WHEN EXISTS(SELECT 1 FROM items i INDEXED BY item_word WHERE i.word_id=words.id AND i.in_scope=1) THEN 1
      WHEN EXISTS(SELECT 1 FROM items i INDEXED BY item_word WHERE i.word_id=words.id) THEN 0 ELSE 1 END''')
    db.execute('''UPDATE subwords SET in_scope=CASE WHEN EXISTS(SELECT 1 FROM items i INDEXED BY item_subword WHERE i.subword_id=subwords.id AND i.in_scope=1) THEN 1
      WHEN EXISTS(SELECT 1 FROM items i INDEXED BY item_subword WHERE i.subword_id=subwords.id) THEN 0 ELSE COALESCE((SELECT in_scope FROM words w WHERE w.id=subwords.word_id),1) END''')
    db.execute("UPDATE pages SET scope_ink_pixels=COALESCE((SELECT SUM(area) FROM items i INDEXED BY item_page WHERE i.page_id=pages.page_id AND i.in_scope=1),0),scope_unknown_pixels=COALESCE((SELECT SUM(area) FROM items i INDEXED BY item_page WHERE i.page_id=pages.page_id AND i.in_scope=1 AND i.final_kind='unknown'),0)")
    source_pixels=db.execute('SELECT SUM(ink_pixels) FROM pages').fetchone()[0]
    kept=sum(r['insidePixels'] for r in results);excluded=sum(r['outsidePixels'] for r in results)
    if source_pixels!=kept+excluded:raise ValueError('Scope accounting lost source ink')
    text_counts=Counter(reason for r in results for reason in r.get('textExclusions',{}).values())
    summary={'version':VERSION,'pages':len(results),'sourceInkPixels':source_pixels,'includedInkPixels':kept,'excludedInkPixels':excluded,
        'includedRegions':db.execute('SELECT COUNT(*) FROM items WHERE in_scope=1').fetchone()[0],
        'excludedRegions':sum(len(r['excludedIds']) for r in results),'partialRegions':0,
        'surahTitleFrames':sum(len(r.get('surahFrames',[])) for r in results),
        'verseMarkers':sum(r.get('verseMarkers',0) for r in results),
        'basmalaOpenings':sum(r.get('basmalaOpenings',0) for r in results),
        'textExclusions':dict(text_counts),'excludedClasses':dict(db.execute('SELECT final_kind,COUNT(*) FROM items WHERE in_scope=0 GROUP BY final_kind')),
        'sourceInkConserved':True,'humanAnnotationsModified':False,'method':'SVG page and surah frames, verse marker geometry and transcript-aligned basmala openings.',
        'specialPages':{r['pageId']:{'framePathId':r['framePathId'],'interiorContours':r['interiorContours']} for r in results if r['interiorContours']!=1}}
    db.execute('INSERT OR REPLACE INTO meta VALUES (?,?)',('scope',json.dumps(summary)))
    record=db.execute("SELECT value FROM meta WHERE key='inference'").fetchone()
    if record:
        inference=json.loads(record[0])
        inference.update(
            sources=dict(db.execute('SELECT final_source,COUNT(*) FROM items WHERE in_scope=1 GROUP BY final_source')),
            classes=dict(db.execute('SELECT final_kind,COUNT(*) FROM items WHERE in_scope=1 GROUP BY final_kind')),
            structuralFlags=db.execute('SELECT COUNT(*) FROM items WHERE in_scope=1 AND prediction_guard IS NOT NULL').fetchone()[0])
        db.execute("UPDATE meta SET value=? WHERE key='inference'",(json.dumps(inference),))
    db.commit()
    browse_indexes(db)
    db.execute('PRAGMA wal_checkpoint(TRUNCATE)');db.close()
    target=root/'scope.json';temp=target.with_suffix('.tmp');temp.write_text(json.dumps(summary,indent=2));temp.replace(target)
    progress('scope-complete',len(results),len(results),{'includedRegions':summary['includedRegions'],'excludedRegions':summary['excludedRegions']})
    return summary


def browse_indexes(db):
    definitions={'item_width_browse':['in_scope','w DESC','id','h','area','components','quality','model_confidence'],
        'item_label_summary':['in_scope','teacher_kind','teacher_type','prediction_kind','prediction_type','final_kind','final_type','model_agrees','refined','subword_id']}
    for source,role,type in [('final','final_kind','final_type'),('teacher','teacher_kind','teacher_type'),('model','prediction_kind','prediction_type')]:
        definitions[f'item_{source}_browse']=['in_scope',role,type,'w','id']
    for name,columns in definitions.items():
        existing=[r[2] for r in db.execute(f'PRAGMA index_info({name})')]
        if existing and existing!=[c.split()[0] for c in columns]:db.execute(f'DROP INDEX {name}')
        db.execute(f'CREATE INDEX IF NOT EXISTS {name} ON items({",".join(columns)})')
    # Supported-result filters are represented directly in the index, avoiding
    # a lookup into the large mask table for every candidate during counts/sorts.
    for source,role,type in [('final','final_kind','final_type'),('model','prediction_kind','prediction_type')]:
        supported="(model_confidence>=0.90 AND prediction_guard IS NULL)"
        if source=='final':supported="((human_reviewed=1 AND origin='human-reviewed') OR "+supported+")"
        db.execute(f'CREATE INDEX IF NOT EXISTS item_{source}_supported_browse ON items(in_scope,{role},{type},w,id,h,area,components,quality,model_confidence) WHERE {supported}')
    db.commit()


def load_scope(root,page_id,source_sha=None):
    root=Path(root)
    if not (root/'scope.json').exists():return None
    manifest=json.loads((root/'scope.json').read_text())
    target=assets_folder(root,manifest['version'])/f'{page_id}.json'
    if not target.exists():return None
    scope=json.loads(target.read_text())
    if source_sha is not None and scope['sourceSha256']!=source_sha:raise ValueError('Scope belongs to a different source SVG')
    return scope


def scoped_review(reconciled,doc,scope):
    if not scope:return reconciled
    allowed=scope['runs'];starts=allowed[::2]
    parts={p['id']:bool(clip_runs(p['runs'],allowed,starts)) for p in doc['parts']}
    paths={p['id']:p for p in reconciled['paths']}
    for c in reconciled['candidates']:
        c['inScope']=any(parts.get(id,False) for id in c['regionIds']) if c['regionIds'] else any(clip_runs(paths[id]['runs'],allowed,starts) for id in c['pathIds'])
    candidates={c['id']:c for c in reconciled['candidates']}
    for s in reconciled['subwords']:s['inScope']=any(candidates[id]['inScope'] for id in s['candidateIds'] if id in candidates)
    reconciled['sourceScope']=scope
    reconciled['alignment']['excludedByFrame']=sum(not value for value in parts.values())
    return reconciled
