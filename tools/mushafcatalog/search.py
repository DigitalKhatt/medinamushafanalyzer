"""Read-only catalog queries. A transcript owner exists independently of its optional body mask."""
import json
import re
import unicodedata
from html import escape
from math import hypot

from store import INK_REF, META, meta, plain, report_ink, require, shape_class, unpack_runs
from page_view import decode_vectors, mark_outlines, matching_part
from automatic.common import mask_path

KINDS = ('subwords', 'words', 'letters', 'marks', 'parts')
ADDRESS = re.compile(r'page(\d{1,3})(?::(l\d+-w\d+)(-s\d+|:(?:c|sign)\d+(?::(?:g\d+|missing))?)?)?')
FORMS = ('isolated','initial','medial','final','unknown')
KASHIDA_METRICS = {'kashidaLeft','kashidaRight','kashida'}
# The corpus search accepts ordinary keyboard diacritics. Match their native
# transcript identity, including edition-specific spellings and carrier letters.
TYPED_MARK = {
    '\u064b':'tanwin-fath', '\u064c':'tanwin-damm', '\u064d':'tanwin-kasr',
    '\u064e':'fatha', '\u064f':'damma', '\u0650':'kasra', '\u0651':'shadda',
    '\u0652':'sukun', '\u06e1':'sukun', '\u0653':'maddah', '\u0654':'hamza-above',
    '\u0655':'hamza-below', '\u0670':'small-alef', '\u08f0':'tanwin-fath',
    '\u08f1':'tanwin-damm', '\u08f2':'tanwin-kasr',
}
WORD_COVERAGE = """(SELECT CASE WHEN count(*)=0 THEN 'sign'
    WHEN count(b.subword_id)=0 THEN 'pending' WHEN count(b.subword_id)=count(*) THEN 'segmented'
    ELSE 'partial' END FROM catalog_subwords s LEFT JOIN catalog_splits b ON b.subword_id=s.id
    WHERE s.word_id=w.id AND s.kind='text')"""
SUB_COVERAGE = "CASE WHEN s.kind!='text' THEN 'sign' WHEN b.subword_id IS NULL THEN 'pending' ELSE 'segmented' END"


def facets(db):
    info = meta(db,META)
    require(info, 'Build the native catalog first.')
    def distinct(table,col):
        return [r[0] for r in db.execute(f'SELECT DISTINCT {col} FROM {table} WHERE {col} IS NOT NULL ORDER BY 1')]
    n = dict(info['inventory'])
    n['segmented'] = db.execute('SELECT count(*) FROM catalog_splits').fetchone()[0]
    n['pending'] = n['subwords']-n['segmented']
    n['parts'] = db.execute('SELECT count(*) FROM catalog_parts').fetchone()[0]
    return dict(inventory=n,expected=distinct('native_marks','expected_type'),predicted=distinct('native_marks','classifier_type'),
                classes=distinct('catalog_owners','shape_class'),forms=distinct('catalog_owners','form'),
                qualities=distinct('catalog_measurements','quality'))


def number(params,key,default,lo,hi):
    return_value = int(params[key]) if params.get(key) else default
    require(lo<=return_value<=hi,f'{key} must be between {lo} and {hi}')
    return return_value


def letter_sequence(value):
    require(len(value)<=240,'Search text is too long')
    slots = []
    for ch in value:
        if ch.isspace():
            continue
        if unicodedata.category(ch).startswith('M') or ch in 'ۥۦ':
            require(slots, 'Put a base letter before its marks in a letter sequence.')
            slots[-1][1].append(ch)
        else:
            require(unicodedata.category(ch).startswith('L'), 'Enter letters in writing order, with optional marks.')
            slots.append((ch,[]))
    require(len(slots)<=8, 'Use up to eight letters per sequence.')
    return slots


def letter_mark_exists(owners, predicate):
    # Anchor each lookup to the selected letter. Starting from mark type can
    # rescan thousands of marks for each candidate letter in a corpus search.
    return ('(EXISTS(SELECT 1 FROM native_marks m INDEXED BY native_marks_owner '
            f'WHERE m.owner_id IN ({owners}) AND {predicate}) OR '
            'EXISTS(SELECT 1 FROM native_marks m INDEXED BY native_marks_attached_letter '
            f'WHERE m.attached_letter_id IS NOT NULL AND m.attached_letter_id IN ({owners}) AND {predicate}))')


def search(db, params, statistics=False):
    kind = params.get('kind','subwords')
    require(kind in KINDS, 'Unknown search kind')
    q = params.get('q','').strip()
    require(len(q)<=240,'Search text is too long')
    sequence = params.get('sequence','').strip()
    if kind=='letters' and q and not ADDRESS.fullmatch(q) and not sequence:
        sequence, q = q, ''
    slots = letter_sequence(sequence) if sequence else []
    require(not slots or kind=='letters', 'A sequence is a letter search')
    require(params.get('match','') in ('','letter','exact','class'), 'Invalid letter matching mode')
    args, where = [], []
    def condition(sql,*values):
        where.append(sql)
        args.extend(values)

    joined = 'words w'
    coverage = WORD_COVERAGE if kind=='words' else SUB_COVERAGE
    if kind=='words':
        joined += ' JOIN catalog_word_bounds wb ON wb.word_id=w.id'
        identifier, text, width, height, order = 'w.id','w.text','wb.w','wb.h','w.page_id,w.line,w.ordinal'
        extra = ''
    else:
        joined = 'catalog_subwords s JOIN words w ON w.id=s.word_id LEFT JOIN catalog_splits b ON b.subword_id=s.id'
        identifier, text, width, height, order = 's.id','s.text','s.w','s.h','s.seq'
        extra = ',s.id subword_id,s.kind category,s.letter_count,s.mask_count'
    if kind in ('letters','parts','marks'):
        joined += ' JOIN catalog_owners o ON o.subword_id=s.id'
        base = 'm.base_character' if kind=='marks' else 'o.character'
        base_offset = 'm.base_offset' if kind=='marks' else 'o.word_offset'
        extra += f',o.id owner_id,o.role owner_role,{base} base,o.form,o.shape_class,{base_offset} base_offset'
        if kind=='letters':
            condition("o.role='letter'")
            joined += " LEFT JOIN catalog_parts p ON p.owner_id=o.id AND p.role='body'"
            joined += ' LEFT JOIN catalog_measurements a ON a.owner_id=o.id'
            extra += ',a.quality,a.confidence'
            identifier, text, width, height, order = 'o.id','o.text','p.w','p.h','o.seq'
        elif kind=='parts':
            joined += ' JOIN catalog_parts p ON p.owner_id=o.id'
            identifier, text, width, height, order = "p.owner_id||'/'||p.role",'o.text','p.w','p.h','o.seq,p.role'
            extra += ',p.role,p.variant,p.next_owner_id,p.area'
        else:
            joined += ' JOIN native_marks m ON m.owner_id=o.id'
            identifier, text, width, height, order = 'm.id','o.text','m.w','m.h','m.seq'
            extra += (',m.character_id,m.word_offset,m.source_character,m.codepoint,m.expected_type,m.classifier_type,'
                      'm.verdict,m.method,m.score,m.raw_type,m.path_ids,m.attached_letter_id')
    group_form = 'o.form'
    aliases, masks = ['o'], ['p']
    if slots:
        for i,(character,marks) in enumerate(slots):
            alias, part = ('o','p') if i==0 else (f'o{i}',f'p{i}')
            if i:
                joined += (f" JOIN catalog_owners {alias} INDEXED BY catalog_owner_sub ON {alias}.subword_id=s.id AND {alias}.role='letter'"
                           f" AND {alias}.ordinal=o.ordinal+{i}"
                           f" LEFT JOIN catalog_parts {part} ON {part}.owner_id={alias}.id AND {part}.role='body'")
                aliases.append(alias)
                masks.append(part)
            if params.get('match')=='class':
                classes = sorted({shape_class(character,'initial'),shape_class(character,'isolated')})
                condition(f"{alias}.shape_class IN ({','.join('?' for c in classes)})",*classes)
            else:
                condition(f'{alias}.character=?',character)
            for mark in marks:
                native_type = TYPED_MARK.get(mark)
                col = 'expected_type' if native_type else 'source_character'
                condition(letter_mark_exists(alias+'.id',f'm.{col}=?'),native_type or mark,native_type or mark)
        extra += ','+"||','||".join(a+'.id' for a in aliases)+' sequence_ids'
        text = '||'.join(a+'.text' for a in aliases)
        if len(masks)>1:
            # Ordinals are word-relative. A run's form describes its position
            # in this particular subword, even if a native letter form is unknown.
            start = "(SELECT min(g.ordinal) FROM catalog_owners g WHERE g.subword_id=s.id AND g.role='letter')"
            group_form = (f"CASE WHEN s.letter_count={len(slots)} THEN 'isolated' WHEN o.ordinal={start} THEN 'initial' "
                          f"WHEN {aliases[-1]}.ordinal={start}+s.letter_count-1 THEN 'final' ELSE 'medial' END")
            for axis,size in [('x','w'),('y','h')]:
                span = 'max('+','.join(f'{p}.{axis}+{p}.{size}' for p in masks)+')-min('+','.join(f'{p}.{axis}' for p in masks)+')'
                if axis=='x': width = span
                else: height = span
    if kind=='letters':
        extra += (f',{group_form} group_form,o.word_offset run_start,'
                  f'{aliases[-1]}.word_offset+length({aliases[-1]}.text) run_end,'
                  f'(SELECT g.character FROM catalog_owners g WHERE g.subword_id=s.id AND g.role=\'letter\' '
                  f'AND g.ordinal={aliases[-1]}.ordinal+1) pair_next')
    page = params.get('page','').strip()
    if page:
        match = re.fullmatch(r'(\d{1,3})(?:\s*-\s*(\d{1,3}))?',page)
        require(match,'Use a page or page range, such as 3 or 1-10')
        first,last = int(match[1]),int(match[2] or match[1])
        require(1<=first<=last<=604,'Pages range from 1 to 604')
        condition('w.page_id BETWEEN ? AND ?',f'page{first:03d}',f'page{last:03d}')
    for term in (q, params.get('within','').strip()):
        if not term: continue
        require(len(term)<=240,'Search text is too long')
        address = ADDRESS.fullmatch(term)
        path = re.fullmatch(r'(page\d{3}):(l\d+-w\d+-p\d+(?:-[\w-]+)?)',term)
        if path and kind=='marks':
            condition('w.page_id=?',path[1])
            exact = json.dumps(path[2])
            condition('(instr(m.path_ids,?)>0 OR instr(m.path_ids,?)>0)',exact,exact[:-1]+'-')
        elif address:
            pid = f'page{int(address[1]):03d}'
            condition('w.page_id=?',pid)
            if address[2]:
                wid = pid+':'+address[2]
                condition('w.id=?',wid)
                suffix = address[3] or ''
                if suffix.startswith('-s'):
                    if kind=='words':
                        condition('EXISTS(SELECT 1 FROM catalog_subwords WHERE id=?)',wid+suffix)
                    else:
                        condition('s.id=?',wid+suffix)
                elif suffix.startswith(':'):
                    full = wid+suffix
                    if kind=='marks':
                        if suffix.count(':')==2: condition('m.id=?',full)
                        else: condition('(m.owner_id=? OR m.character_id=? OR m.attached_letter_id=?)',full,full,full)
                    elif kind in ('letters','parts'):
                        condition('(o.id=? OR o.id IN (SELECT coalesce(attached_letter_id,owner_id) '
                                  'FROM native_marks WHERE character_id=? OR id=?))',full,full,full)
                    elif kind=='subwords': condition('s.id IN (SELECT subword_id FROM catalog_owners WHERE id=? UNION SELECT subword_id FROM native_marks WHERE character_id=? OR id=?)',full,full,full)
                    else: condition('EXISTS(SELECT 1 FROM catalog_owners WHERE id=? UNION ALL SELECT 1 FROM native_marks WHERE character_id=? OR id=?)',full,full,full)
        else:
            if kind=='marks' and re.fullmatch(r'U\+[0-9A-Fa-f]{4,6}',term,re.I):
                condition('m.codepoint=?',term.upper())
            elif kind=='marks' and len(term)==1 and (unicodedata.category(term).startswith('M') or term in 'ۥۦ'):
                if term in TYPED_MARK: condition('m.expected_type=?',TYPED_MARK[term])
                else: condition('m.source_character=?',term)
            elif kind=='marks' and re.fullmatch('[a-zA-Z]+(?:[ -][a-zA-Z]+)*',term):
                name = term.lower().replace(' ','-')
                condition('(instr(m.expected_type,?)>0 OR instr(m.classifier_type,?)>0)',name,name)
            else:
                # Marked input is literal. Unvocalized input also searches the folded Arabic spelling.
                needle = plain(term)
                subject = 's.text' if kind=='subwords' else 'w.text'
                folded = 's.bases' if kind=='subwords' else 'w.bases'
                if any(unicodedata.category(c).startswith('M') for c in term) or not needle:
                    condition(f'instr({subject},?)>0',term)
                else:
                    condition(f'(instr({subject},?)>0 OR instr({folded},?)>0)',term,needle)
    if params.get('coverage'):
        require(params['coverage'] in ('pending','partial','segmented','sign'),'Invalid coverage')
        condition(coverage+'=?',params['coverage'])
    if kind=='subwords' and params.get('category'):
        condition('s.kind=?',params['category'])
    if kind=='parts' and params.get('role'):
        condition('p.role=?',params['role'])
    if kind=='marks':
        if params.get('character'): condition('m.character_id=?',params['character'])
        if params.get('owner'):
            condition('(m.owner_id=? OR m.attached_letter_id=?)',params['owner'],params['owner'])
    if kind=='letters' and params.get('quality'):
        condition('a.quality=?',params['quality'])
    if kind in ('letters','marks','parts'):
        for key,column in [('base',base),('class','o.shape_class'),('prev','o.prev_letter')]:
            if params.get(key): condition(column+'=?',params[key])
        last = f'o{len(slots)-1}' if len(slots)>1 else 'o'
        if params.get('next'): condition(last+'.next_letter=?',params['next'])
        forms = list(dict.fromkeys((params.get('forms') or params.get('form') or '').split(',')))
        if forms!=['']:
            require(set(forms)<=set(FORMS),'Unknown joining form')
            condition(f"{group_form} IN ({','.join('?' for _ in forms)})",*forms)
    if params.get('minConfidence'):
        require(kind in ('letters','marks'),'Confidence applies to letters or marks')
        cutoff = float(params['minConfidence'])
        require(0<=cutoff<=1,'Minimum confidence must be between 0 and 1')
        condition(('m.score' if kind=='marks' else 'a.confidence')+'>=?',cutoff)
    mk, mv = [], []
    for key,col in [('expected','expected_type'),('predicted','classifier_type'),('verdict','verdict')]:
        if params.get(key):
            if key=='verdict' and params[key]=='issues':
                mk.append("m.verdict IN ('different','missing','ambiguous','unmatched')")
            else:
                mk.append('m.'+col+'=?')
                mv.append(params[key])
    if mk:
        predicate = ' AND '.join(mk)
        if kind in ('letters','parts'):
            owners = ','.join(a+'.id' for a in aliases)
            predicate = letter_mark_exists(owners,predicate)
            mv = mv * 2
        elif kind!='marks':
            link = {'words':'m.word_id=w.id','subwords':'m.subword_id=s.id'}[kind]
            predicate = f'EXISTS(SELECT 1 FROM native_marks m WHERE {link} AND {predicate})'
        condition(predicate,*mv)
    metric = params.get('metric','width')
    measures = {'width':width,'height':height}
    if kind=='letters':
        dims = ['a']
        for i in range(1,len(slots)):
            alias = f'a{i}'
            joined += f' LEFT JOIN catalog_measurements {alias} ON {alias}.owner_id=o{i}.id'
            dims.append(alias)
        measures.update(advance='+'.join(a+'.advance' for a in dims),spine='+'.join(a+'.spine' for a in dims),
                        kashidaLeft=dims[-1]+'.kashida_left',kashidaRight='a.kashida_right')
        # Each internal join is counted once. A missing measurement stays
        # unknown; only a true subword boundary is known to have no connector.
        if metric=='kashida':
            def edge(alias, column, step):
                direction = '+' if step==1 else '-'
                return (f"CASE WHEN NOT EXISTS(SELECT 1 FROM catalog_owners k WHERE k.subword_id=s.id "
                        f"AND k.role='letter' AND k.ordinal={alias}.ordinal{direction}1) THEN 0 ELSE {column} END")
            spans = [edge('o','a.kashida_right',-1),*(a+'.kashida_left' for a in dims[:-1]),
                     edge(aliases[-1],dims[-1]+'.kashida_left',1)]
            measures['kashida'] = 'CASE WHEN b.subword_id IS NOT NULL THEN '+ '+'.join(spans)+' END'
    require(metric in measures, 'Metric unavailable for this search kind')
    measure = measures[metric]
    for k,operator in [('min','>='),('max','<=')]:
        if params.get(k):
            value = float(params[k])
            require(0<=value<=100000,'Measurement outside search range')
            condition('('+measure+')'+operator+'?',value)
    source = ' FROM '+joined+(' WHERE '+' AND '.join(where) if where else '')
    if statistics:
        bins = db.execute('SELECT ('+measure+') v,count(*)'+source+' GROUP BY v ORDER BY v',args).fetchall()
        return dict(metric=metric,unit='raster pixels',unmeasured=sum(n for v,n in bins if v is None),
                    distribution=[[v,n] for v,n in bins if v is not None])
    limit = number(params,'limit',24,1,1000)
    offset = number(params,'offset',0,0,3000000)
    sort = params.get('sort','location')
    require(sort in ('location','largest','smallest','confidence'),'Unknown sort')
    if sort=='confidence':
        require(kind in ('letters','marks'),'Confidence is available for marks and letter cuts')
        score = 'm.score' if kind=='marks' else 'a.confidence'
        order = f'{score} IS NULL,{score},'+order
    elif sort!='location':
        order = 'metric_value IS NULL,metric_value '+('DESC' if sort=='largest' else 'ASC')+','+order
    total = db.execute('SELECT count(*)'+source,args).fetchone()[0]
    columns = (identifier+' id,'+text+' text,w.id word_id,w.text word_text,w.page_id,w.line,w.ordinal word_number,'+
               coverage+' coverage,'+width+' width,'+height+' height,('+measure+') metric_value'+extra)
    rows = [dict(r) for r in db.execute('SELECT '+columns+source+' ORDER BY '+order+' LIMIT ? OFFSET ?',args+[limit,offset])]
    result = dict(kind=kind,total=total,limit=limit,offset=offset,rows=rows)
    if not total and metric in KASHIDA_METRICS and (params.get('min') or params.get('max')):
        recorded = db.execute('SELECT 1 FROM catalog_measurements WHERE kashida_left IS NOT NULL OR kashida_right IS NOT NULL LIMIT 1').fetchone()
        result['emptyMessage'] = ('No measured kashidas match these filters.' if recorded else
            'This catalog has no kashida lengths yet. Run the “Segment letters and update database” task, then search again.')
    return result


def detail(db, wid):
    word = db.execute('SELECT id,page_id,line,ordinal,text FROM words WHERE id=?',(wid,)).fetchone()
    if not word: raise KeyError('Unknown word')
    result = dict(word)
    subs = [dict(r) for r in db.execute('SELECT s.id,s.text,s.kind,s.members,s.letter_count,s.mask_count,s.body_key,s.body_path,'
            +SUB_COVERAGE+' coverage,b.producer FROM catalog_subwords s LEFT JOIN catalog_splits b ON b.subword_id=s.id '
            'WHERE s.word_id=? ORDER BY s.ordinal',(wid,))]
    for s in subs:
        s['members'] = json.loads(s['members'])
        s['owners'] = [dict(r) for r in db.execute('''SELECT id,role,text,character,word_offset,form,shape_class
                FROM catalog_owners WHERE subword_id=? ORDER BY word_offset''',(s['id'],))]
        for o in s['owners']:
            o['parts'] = [dict(r) for r in db.execute('SELECT role,next_owner_id,variant,w,h,area FROM catalog_parts WHERE owner_id=? ORDER BY role',(o['id'],))]
            metrics = db.execute('SELECT advance,spine,kashida_left,kashida_right,confidence,quality FROM catalog_measurements WHERE owner_id=?',(o['id'],)).fetchone()
            if metrics: o['metrics'] = {k:v for k,v in dict(metrics).items() if v is not None}
            o['marks'] = [dict(r) for r in db.execute('''SELECT id,character_id,word_offset,source_character,codepoint,
                expected_type,classifier_type,raw_type,method,equivalence,verdict,score,path_ids,attached_letter_id FROM native_marks
                WHERE owner_id=? ORDER BY word_offset,seq''',(o['id'],))]
            for m in o['marks']: m['path_ids'] = json.loads(m['path_ids'] or '[]')
    result['subwords'] = subs
    return result


def body_source(db, sid):
    row = db.execute('''SELECT s.id,s.word_id,s.text,s.body_key,s.body_path,s.runs,c.raster,c.pdf_to_raster,
        p.pdf_sha,p.source_sha FROM catalog_subwords s JOIN catalog_coordinates c USING(page_id)
        JOIN pages p USING(page_id) WHERE s.id=? AND s.kind='text' ''',(sid,)).fetchone()
    if not row: raise KeyError('Unknown letter-bearing subword')
    owners = [dict(r) for r in db.execute("""SELECT id,word_offset,character,text,form,shape_class FROM catalog_owners
        WHERE subword_id=? AND role='letter' ORDER BY word_offset""",(sid,))]
    blob = row['runs']
    runs = report_ink(db,sid[:7])[0].get(sid,[]) if blob==b'' else unpack_runs(blob) if blob else []
    return dict(subword=sid, word=row['word_id'], text=row['text'], sourceKey=row['body_key'], path=row['body_path'],
                raster=json.loads(row['raster']), pdfToRaster=json.loads(row['pdf_to_raster']), pdfSha256=row['pdf_sha'],
                svgSha256=row['source_sha'], runs=runs, owners=owners)


def image(db, kind, identifier, context, *, pair=False, left=False, right=False, dark=False):
    require(kind in KINDS and (context in ('word','crop') or (kind,context) in (('marks','letter'),('letters','match'))),
            'Unknown image kind or context')
    # A sequence preview uses the same verified contiguous transcript owners returned by search.
    focus = identifier.split(',')
    require(1<=len(focus)<=8 and len(identifier)<=700,'Invalid image identifier')
    wid = re.match(r'^(page\d{3}:l\d+-w\d+)(?=$|[-:/])',focus[0])
    if not wid: raise KeyError('Unknown word address')
    wid = wid[1]
    word = db.execute('''SELECT w.x,w.y,w.w,w.h,p.width,n.runs FROM catalog_word_bounds w JOIN pages p USING(page_id)
        LEFT JOIN native_mark_words n USING(word_id) WHERE w.word_id=?''',(wid,)).fetchone()
    if not word: raise KeyError('Unknown word')
    x,y,w,h,width,wruns = word
    art = None
    if db.execute("SELECT 1 FROM sqlite_master WHERE name='catalog_page_art'").fetchone():
        art = db.execute('''SELECT a.vectors FROM catalog_page_art a
            JOIN pages p USING(page_id) JOIN catalog_pages c USING(page_id)
            WHERE a.page_id=? AND a.source_sha=p.source_sha AND a.report_sha=c.report_sha''',(wid[:7],)).fetchone()
    vectors = decode_vectors(art[0] if art else None)
    raster_marks = set(vectors.get('rasterMarks', []))
    layers = []
    body, mark, muted, connector, neighbour = (('#edf4fa','#ffc267','#5d7387','#72c7ff','#99aebf') if dark else
                                                ('#244037','#ad542f','#c6cec4','#008fdf','#85978c'))

    def layer(runs,color,outlines=()):
        if outlines:
            for outline in outlines:
                d = escape(outline['d'],quote=True)
                rule = escape(outline.get('fillRule','nonzero'),quote=True)
                layers.append(f'<path fill="{color}" fill-rule="{rule}" d="{d}"/>')
        elif runs and not runs.startswith(INK_REF):
            layers.append(f'<path fill="{color}" d="{mask_path(unpack_runs(runs),width)}"/>')

    def word_layer(color):
        prefix = wid.split(':',1)[1]+'-p'
        outlines = [path for name,path in vectors['paths'].items()
                    if name.startswith(prefix) and name[len(prefix):].isdigit()]
        if outlines or wruns:
            layer(wruns,color,outlines)
        else:
            for (runs,) in db.execute('SELECT runs FROM catalog_subwords WHERE word_id=?',(wid,)):
                layer(runs,color)

    def mark_layer(mark_id,ids,runs,color=mark):
        if runs is not None:
            layer(runs,color,mark_outlines(vectors,raster_marks,mark_id,ids))

    def part_layer(owner,role,runs,color):
        outline = matching_part(vectors,owner,role,runs)
        layer(runs,color,[outline] if outline else [])

    def subword_layer(sid,runs,color):
        # A complete partition also covers repaired compound bodies. A single
        # source path ID may omit some of that ink.
        parts = [(blob,matching_part(vectors,owner,role,blob)) for owner,role,blob in db.execute(
            'SELECT owner_id,role,runs FROM catalog_parts WHERE subword_id=?',(sid,))]
        if any(outline for _,outline in parts):
            for blob,outline in parts:
                layer(blob,color,[outline] if outline else [])
        else:
            if runs==b'':
                _,_,groups = report_ink(db,sid[:7])
                outlines = [vectors['paths'].get(i) for i in groups.get(sid,[])]
                layer(None,color,outlines if all(outlines) else [])
            else:
                layer(runs,color)

    if kind=='words':
        if identifier!=wid: raise KeyError('Unknown word')
        word_layer(body)
    elif kind=='marks':
        show_inset = context=='letter'
        m = db.execute('''SELECT x,y,w,h,runs,path_ids,coalesce(attached_letter_id,owner_id)
            FROM native_marks WHERE id=? AND word_id=?''',(identifier,wid)).fetchone()
        if not m: raise KeyError('Unknown mark')
        if context=='letter':
            owner = m[6]
            sub = db.execute('''SELECT s.id,s.x,s.y,s.w,s.h,s.runs FROM catalog_owners o
                JOIN catalog_subwords s ON s.id=o.subword_id WHERE o.id=? AND o.role='letter' ''',(owner,)).fetchone()
            if sub:
                part = db.execute("""SELECT x,y,w,h,runs FROM catalog_parts
                    WHERE owner_id=? AND role='body'""",(owner,)).fetchone()
                if part:
                    part_layer(owner,'body',part[4],body)
                    bounds = [part[:4]]
                else:
                    subword_layer(sub[0],sub[5],muted)
                    bounds = [sub[1:5]]
                for mid,ids,runs,mx,my,mw,mh in db.execute('''SELECT id,path_ids,runs,x,y,w,h FROM native_marks
                    WHERE (owner_id=? OR attached_letter_id=?) AND id!=? AND runs IS NOT NULL
                    ORDER BY seq''',(owner,owner,identifier)):
                    mark_layer(mid,ids,runs,neighbour)
                    bounds.append((mx,my,mw,mh))
                if m[4] is not None:
                    bounds.append(m[:4])
                x,y = min(box[0] for box in bounds),min(box[1] for box in bounds)
                w = max(bx+bw for bx,_,bw,_ in bounds)-x
                h = max(by+bh for _,by,_,bh in bounds)-y
            else:
                context = 'word'
        if m[4] is not None:
            if context=='crop': x,y,w,h = m[:4]
            mark_layer(identifier,m[5],m[4])
            if show_inset:
                # Repeat the selected outline at a readable size with its own
                # circular frame so identification does not depend on hue.
                start = len(layers)
                mark_layer(identifier,m[5],m[4],'#111')
                if len(layers)>start:
                    mx,my,mw,mh = m[:4]
                    radius = max(12,min(32,h*.31))
                    gap = max(8,radius*.45)
                    cx,cy = x+w+gap+radius,y+h/2
                    scale = radius*1.4/max(1,hypot(mw,mh))
                    isolated = ''.join(layers[start:])
                    layers[start:] = [
                        '<g data-part="selected-mark-inset">'
                        f'<circle cx="{cx:g}" cy="{cy:g}" r="{radius:g}" '
                        'fill="#fff" stroke="#8fa7bd" stroke-width="1.5"/>'
                        f'<g transform="translate({cx:g} {cy:g}) scale({scale:g}) '
                        f'translate({-(mx+mw/2):g} {-(my+mh/2):g})">{isolated}</g></g>'
                    ]
                    w += gap+2*radius
                    y = min(y,cy-radius)
                    h = max(h,2*radius)
        elif context=='crop': context='word'
    elif kind=='parts':
        owner, separator, role = identifier.partition('/')
        part = db.execute('SELECT x,y,w,h,runs FROM catalog_parts WHERE owner_id=? AND role=?',(owner,role)).fetchone()
        if not separator or not part: raise KeyError('Unknown letter part')
        if context=='crop': x,y,w,h = part[:4]
        part_layer(owner,role,part[4],body if role=='body' else connector)
    else:
        if kind=='subwords':
            sid = identifier
        else:
            owners = [db.execute("SELECT subword_id,ordinal FROM catalog_owners WHERE id=? AND word_id=? AND role='letter'",(i,wid)).fetchone() for i in focus]
            if (not all(owners) or len({o[0] for o in owners})!=1 or
                    any(o[1]!=owners[0][1]+i for i,o in enumerate(owners))): raise KeyError('Unknown letter or sequence')
            sid = owners[0][0]
        sub = db.execute('SELECT x,y,w,h,runs,letter_count FROM catalog_subwords WHERE id=? AND word_id=?',(sid,wid)).fetchone()
        if not sub: raise KeyError('Unknown subword')
        if context=='crop': x,y,w,h = sub[:4]
        if kind=='subwords':
            subword_layer(sid,sub[4],body)
            for r in db.execute('SELECT id,path_ids,runs FROM native_marks WHERE subword_id=?',(sid,)): mark_layer(*r)
        else:
            # A complete-subword match owns the entire native body even when
            # individual cuts are pending. Never invent a cut for a partial run.
            complete = sub[5]==len(focus)
            bounds = []

            def draw(renderer, box, *args):
                start = len(layers)
                renderer(*args)
                if context=='match' and len(layers)>start:
                    bounds.append(box)

            if complete or context!='match':
                draw(subword_layer,sub[:4],sid,sub[4],body if complete else muted)
            if pair:
                for r in db.execute("""SELECT p.x,p.y,p.w,p.h,p.owner_id,p.role,p.runs FROM catalog_parts p JOIN catalog_owners n ON n.id=p.owner_id
                       WHERE n.subword_id=? AND n.ordinal=? AND p.role='body'""",(sid,owners[-1][1]+1)):
                    draw(part_layer,r[:4],*r[4:],neighbour)
            if right:
                for r in db.execute("SELECT x,y,w,h,owner_id,role,runs FROM catalog_parts WHERE subword_id=? AND next_owner_id=?",(sid,focus[0])):
                    draw(part_layer,r[:4],*r[4:],connector)
            for i,owner in enumerate(focus):
                for bx,by,bw,bh,role,runs in db.execute('SELECT x,y,w,h,role,runs FROM catalog_parts WHERE owner_id=?',(owner,)):
                    if role=='body' or i<len(focus)-1: draw(part_layer,(bx,by,bw,bh),owner,role,runs,body)
                    elif left: draw(part_layer,(bx,by,bw,bh),owner,role,runs,connector)
                for r in db.execute('SELECT x,y,w,h,id,path_ids,runs FROM native_marks WHERE owner_id=? OR attached_letter_id=?',
                                    (owner,owner)):
                    draw(mark_layer,r[:4],*r[4:])
            if context=='match':
                if bounds:
                    x,y = min(b[0] for b in bounds),min(b[1] for b in bounds)
                    w = max(bx+bw for bx,_,bw,_ in bounds)-x
                    h = max(by+bh for _,by,_,bh in bounds)-y
                else:
                    x,y,w,h = sub[:4]
    if context=='word' and kind!='words':
        foreground, layers = layers, []
        word_layer(muted)
        layers.extend(foreground)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x-5} {y-5} {max(1,w)+10} {max(1,h)+10}"'
            f' role="img"><title>{escape(identifier)}</title>'+''.join(layers)+'</svg>')
