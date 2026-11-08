"""Dependency-free letter search, source-mask rendering and separate review log."""
from contextlib import closing, contextmanager
from datetime import datetime, timezone
import html
import json
from pathlib import Path
import sqlite3

from automatic.common import mask_path, plain, unpack_runs
from automatic.query import number, value

COLORS = ['#2367b2','#b14e36','#138276','#8b4cad','#bb7618','#356c52','#b13a6a']


class Conflict(ValueError):
    pass


class Corpus:
    def __init__(self, root, reviews=None):
        self.root = Path(root).resolve()
        self.path = self.root/'corpus.sqlite'
        if not self.path.exists():
            raise ValueError('Build this corpus first with build_letter_corpus.py --pages 3')
        self.reviews = Path(reviews).resolve() if reviews else self.root/'reviews.sqlite'
        self.reviews.parent.mkdir(parents=True, exist_ok=True)
        with closing(sqlite3.connect(self.reviews)) as db, db:
            db.executescript('''CREATE TABLE IF NOT EXISTS mark_reviews(
                id TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, letter_id TEXT,
                state TEXT NOT NULL, note TEXT NOT NULL, revision INTEGER NOT NULL, updated_at TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS review_events(
                event_id INTEGER PRIMARY KEY, id TEXT NOT NULL, fingerprint TEXT NOT NULL, letter_id TEXT,
                state TEXT NOT NULL, note TEXT NOT NULL, revision INTEGER NOT NULL, updated_at TEXT NOT NULL);''')

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.path.as_uri()+'?mode=ro', uri=True)
        db.row_factory = sqlite3.Row
        db.execute('ATTACH DATABASE ? AS reviews', (self.reviews.as_uri()+'?mode=ro',))
        db.execute('BEGIN')
        try: yield db
        finally: db.close()

    @staticmethod
    def resolve_mark(row):
        m = dict(row)
        stale = bool(m.get('review_revision') and m.get('review_fingerprint') != m['fingerprint'])
        reviewed = m.get('review_state') == 'approved' and not stale
        m.update(owner=m.get('review_letter') if reviewed else m['predicted_letter_id'],
                 attachment_reviewed=reviewed, review_stale=stale)
        m['effective_status'] = 'reviewed' if reviewed else 'stale-review' if stale else m['status']
        m['candidates'] = json.loads(m['candidates'])
        return m

    def marks(self, db, where='1', args=()):
        rows = [self.resolve_mark(r) for r in db.execute(f'''SELECT m.*,s.x,s.y,s.w,s.h,s.area,
            rv.letter_id review_letter,rv.fingerprint review_fingerprint,rv.state review_state,
            rv.revision review_revision,rv.note review_note,rv.updated_at review_updated_at,
            t.slot_id,t.expected_type,t.character,t.source_character,t.word_text_offset,t.dot_count,
            t.status transcript_status,t.candidates transcript_candidates,
            v.classifier_type,v.classifier_confidence,v.verdict validation
            FROM marks m JOIN source_items s ON s.id=m.id
            LEFT JOIN reviews.mark_reviews rv ON rv.id=m.id
            LEFT JOIN mark_transcripts t ON t.mark_id=m.id
            LEFT JOIN mark_validation v ON v.mark_id=m.id WHERE {where} ORDER BY m.id''', args)]
        types = {}
        for r in db.execute('SELECT DISTINCT letter_id,type FROM expected_marks WHERE letter_id IS NOT NULL'):
            types.setdefault(r[0],set()).add(r[1])
        for row in rows:
            row['transcript_candidates']=json.loads(row['transcript_candidates'] or '[]')
            if row['owner']!=row['predicted_letter_id']:
                row.update(transcript_status='owner-changed-since-association',expected_type=None,validation='stale-association')
            row['expected_types'] = sorted(t for t in types.get(row['owner'],())
                if (row['kind']=='dot') == t.endswith('-dot'))
        return rows

    def metadata(self):
        with self.connect() as db:
            model = json.loads(db.execute("SELECT value FROM meta WHERE key='model'").fetchone()[0])
            counts = {t: db.execute(f'SELECT COUNT(*) FROM {t}').fetchone()[0]
                      for t in ('pages','words','subwords','letters','regions','marks')}
            counts.update(unresolvedLetters=db.execute("SELECT COUNT(*) FROM letters WHERE status='unresolved'").fetchone()[0],
                unresolvedSubwords=db.execute("SELECT COUNT(*) FROM subwords WHERE status='unresolved'").fetchone()[0],
                unassignedInk=db.execute("SELECT COALESCE(SUM(area),0) FROM regions WHERE role='unknown'").fetchone()[0])
            marks = self.marks(db)
            counts.update(reviewedMarks=sum(m['attachment_reviewed'] for m in marks),
                transcriptTypedMarks=sum(m['expected_type'] is not None for m in marks),
                classifierDifferences=sum(m['validation']=='difference' for m in marks),
                proposedMarks=sum(m['effective_status']=='proposed' for m in marks),
                unresolvedMarks=sum(m['effective_status'] in ('unresolved','stale-review') for m in marks),
                wordSigns=sum(m['effective_status']=='word-sign' for m in marks))
            return dict(model=model, counts=counts,
                pages=[dict(r) for r in db.execute('SELECT id,split,width,height FROM pages ORDER BY id')],
                characters=[r[0] for r in db.execute('SELECT DISTINCT char FROM letters ORDER BY char')],
                classes=[r[0] for r in db.execute('SELECT DISTINCT shape_class FROM letters ORDER BY shape_class')],
                markTypes=[r[0] for r in db.execute('SELECT DISTINCT type FROM expected_marks ORDER BY type')],
                corpus=str(self.path), reviews=str(self.reviews))

    def search(self, params):
        kind = value(params, 'kind', 'letters')
        if kind not in ('letters','subwords','marks'): raise ValueError('Invalid search kind')
        limit = number(params, 'limit', 36, integer=True, lo=1, hi=120)
        offset = number(params, 'offset', 0, integer=True, lo=0)
        clauses, args = ['1'], []
        query = value(params, 'q').strip()
        if len(query) > 256: raise ValueError('Search text is too long')
        if query:
            escaped = plain(query).replace('\\','\\\\').replace('%','\\%').replace('_','\\_')
            clauses.append("(w.bases LIKE ? ESCAPE '\\' OR t.id LIKE ? ESCAPE '\\')")
            args.extend(['%'+escaped+'%']*2)
        page = value(params, 'page')
        if page:
            clauses.append('t.page_id=?'); args.append(page)
        if kind == 'letters':
            for key, column in [('char','char'),('form','form'),('class','shape_class')]:
                if value(params,key): clauses.append(f't.{column}=?'); args.append(value(params,key))
        status = value(params, 'status')
        if status and kind != 'marks': clauses.append('t.status=?'); args.append(status)
        if value(params, 'type'):
            if kind == 'marks': clauses.append('t.type=?')
            else:
                column='letter_id' if kind=='letters' else 'subword_id'
                clauses.append(f'EXISTS (SELECT 1 FROM expected_marks m WHERE m.{column}=t.id AND m.type=?)')
            args.append(value(params,'type'))
        sorts = {'reading':'w.page_id,w.line,w.ordinal,t.ordinal,t.id','width':'t.w DESC,t.id',
                 'height':'t.h DESC,t.id','area':'t.area DESC,t.id'}
        sort = value(params, 'sort', 'reading')
        if sort not in sorts: raise ValueError('Invalid sort')
        if kind=='subwords' and sort=='area': order='t.w*t.h DESC,t.id'
        else: order=sorts[sort]
        with self.connect() as db:
            if kind == 'marks':
                # Mark review state lives separately from the disposable index.
                rows = self.marks(db)
                word_info = {r['id']:dict(r) for r in db.execute('SELECT id,text,bases,line,ordinal FROM words')}
                for r in rows: r.update(text=word_info.get(r['word_id'],{}).get('text',''))
                rows = [r for r in rows if (not page or r['page_id']==page)
                    and (not query or plain(query) in plain(r['text']) or query in r['id'])
                    and (not status or r['effective_status']==status or status=='type-unresolved' and not r['expected_type']
                         or status=='classifier-difference' and r['validation']=='difference')
                    and (not value(params,'type') or value(params,'type')==r['expected_type']
                         or not r['expected_type'] and value(params,'type') in r['expected_types'])]
                rows.sort(key=lambda r: (-r[{'width':'w','height':'h','area':'area'}[sort]],r['id']) if sort!='reading'
                    else (r['page_id'],word_info.get(r['word_id'],{}).get('line',0),word_info.get(r['word_id'],{}).get('ordinal',0),r['id']))
                return dict(total=len(rows), rows=rows[offset:offset+limit], offset=offset, limit=limit)
            sql = f"FROM {kind} t JOIN words w ON w.id=t.word_id WHERE {' AND '.join(clauses)}"
            total = db.execute('SELECT COUNT(*) '+sql,args).fetchone()[0]
            rows = [dict(r) for r in db.execute('SELECT t.*,w.text word_text,w.line,w.ordinal word_index '+sql+
                                                f' ORDER BY {order} LIMIT ? OFFSET ?', args+[limit,offset])]
            for r in rows:
                if 'diagnostics' in r: r['diagnostics']=json.loads(r['diagnostics'])
            return dict(total=total, rows=rows, offset=offset, limit=limit)

    def detail(self, kind, identifier):
        if kind not in ('letters','subwords','marks'): raise ValueError('Invalid entity')
        with self.connect() as db:
            row = db.execute(f'SELECT * FROM {kind} WHERE id=?', (identifier,)).fetchone()
            if not row: raise KeyError('Item not found')
            selected = dict(row)
            word = db.execute('SELECT * FROM words WHERE id=?',(selected['word_id'],)).fetchone()
            letters = [dict(r) for r in db.execute('SELECT * FROM letters WHERE word_id=? ORDER BY word_ordinal',(selected['word_id'],))]
            subs = [dict(r) for r in db.execute('SELECT * FROM subwords WHERE word_id=? ORDER BY ordinal',(selected['word_id'],))]
            for s in subs: s['diagnostics']=json.loads(s['diagnostics'])
            sid = selected['id'] if kind=='subwords' else selected['subword_id']
            slots = [dict(r) for r in db.execute('SELECT * FROM expected_marks WHERE subword_id=?',(sid,))]
            return dict(kind=kind, selected=selected, word=dict(word) if word else None, letters=letters,
                subwords=subs, marks=self.marks(db,'m.word_id=?',(selected['word_id'],)), expected=slots,
                regions=[dict(r) for r in db.execute('SELECT id,subword_id,role,letter_id,x,y,w,h,area,raw_area '
                    'FROM regions WHERE subword_id IN (SELECT id FROM subwords WHERE word_id=?)',(selected['word_id'],))])

    def image(self, kind, identifier, context=False, raw=False):
        detail = self.detail(kind, identifier)
        sel, word = detail['selected'], detail['word']
        with self.connect() as db:
            page = db.execute('SELECT width FROM pages WHERE id=?',(sel['page_id'],)).fetchone()
            width = page[0]
            sources = [dict(r) for r in db.execute('SELECT * FROM source_items WHERE word_id=?',(sel['word_id'],))]
            rs = [dict(r) for r in db.execute('SELECT * FROM regions WHERE subword_id IN '
                '(SELECT id FROM subwords WHERE word_id=?)',(sel['word_id'],))]
        sid = sel['id'] if kind=='subwords' else sel['subword_id']
        selected_marks = {m['id'] for m in detail['marks'] if
            (kind=='marks' and m['id']==identifier or kind=='letters' and m['owner']==identifier or
             kind=='subwords' and m['subword_id']==identifier)}
        selected_regions = {r['id'] for r in rs if
            (kind=='letters' and r['letter_id']==identifier or kind=='subwords' and r['subword_id']==identifier)}
        if kind=='marks':
            owner = next(m['owner'] for m in detail['marks'] if m['id']==identifier)
            selected_regions = {r['id'] for r in rs if r['letter_id']==owner and owner} if context else set()
        visible = sources if context else ([r for r in rs if r['id'] in selected_regions]+[s for s in sources if s['id'] in selected_marks])
        if not visible:
            visible = [s for s in sources if s['subword_id']==sid]
        if not visible:
            return '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 160 60"><text x="8" y="32">Unresolved body</text></svg>'
        x, y = min(r['x'] for r in visible)-7, min(r['y'] for r in visible)-7
        w, h = max(r['x']+r['w'] for r in visible)-x+7, max(r['y']+r['h'] for r in visible)-y+7
        out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x} {y} {w} {h}" role="img">',
               f'<title>{html.escape(identifier)}</title><rect x="{x}" y="{y}" width="{w}" height="{h}" fill="white"/>']
        def draw(runs, color):
            out.append(f'<path fill="{color}" d="{mask_path(unpack_runs(runs),width)}"/>')
        for s in sources:
            if context or s['subword_id']==sid: draw(s['runs'],'#d9dce0')
        letter_colors = {l['id']:COLORS[(l['word_ordinal']-1)%len(COLORS)] for l in detail['letters']}
        for r in rs:
            if r['id'] not in selected_regions: continue
            fallback = COLORS[(int(r['id'].rsplit('region',1)[1])-1)%len(COLORS)] if r['role']=='body' else '#9b9b9b'
            color = '#bc7b18' if r['role']=='kashida' else letter_colors.get(r['letter_id'],fallback)
            draw(r['raw_runs'] if raw else r['runs'], color)
        for m in detail['marks']:
            if m['id'] not in selected_marks: continue
            source = next(s for s in sources if s['id']==m['id'])
            draw(source['runs'], '#008573' if kind=='marks' else letter_colors.get(m['owner'],'#bd4b64'))
        out.append('</svg>')
        return ''.join(out)

    def review(self, payload):
        identifier = payload.get('id')
        if payload.get('state') not in ('approved','reopened'): raise ValueError('Invalid review state')
        if type(payload.get('revision')) is not int: raise ValueError('Missing review revision')
        note = payload.get('note','')
        if not isinstance(note,str) or len(note)>4000: raise ValueError('Invalid review note')
        with self.connect() as db:
            marks = self.marks(db,'m.id=?',(identifier,))
            if not marks: raise ValueError('Mark not found')
            mark = marks[0]
            if payload.get('fingerprint') != mark['fingerprint']:
                raise Conflict('The source or segmentation changed; reload before reviewing')
            owner = payload.get('letter_id') if payload['state']=='approved' else None
            if owner:
                found = db.execute('SELECT subword_id FROM letters WHERE id=?',(owner,)).fetchone()
                if not found or found[0] != mark['subword_id']:
                    raise ValueError('Choose a letter in this subword; upstream ownership is reviewed separately')
        with closing(sqlite3.connect(self.reviews, timeout=10)) as db, db:
            db.execute('BEGIN IMMEDIATE')
            current = db.execute('SELECT revision FROM mark_reviews WHERE id=?',(identifier,)).fetchone()
            revision = current[0] if current else 0
            if payload['revision'] != revision: raise Conflict('This attachment was edited elsewhere; reload it')
            revision += 1
            values = (identifier,mark['fingerprint'],owner,payload['state'],note,revision,datetime.now(timezone.utc).isoformat())
            db.execute('INSERT OR REPLACE INTO mark_reviews VALUES (?,?,?,?,?,?,?)',values)
            db.execute('INSERT INTO review_events(id,fingerprint,letter_id,state,note,revision,updated_at) VALUES (?,?,?,?,?,?,?)',values)
        return dict(id=identifier,revision=revision,state=payload['state'])
