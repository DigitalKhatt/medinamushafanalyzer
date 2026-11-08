"""Read-only search of native mark assignments inside the all-page vector corpus."""
from html import escape
from functools import lru_cache
import json
from pathlib import Path
import re
import unicodedata
import zlib

from .common import mask_path, plain, unpack_runs
from .query import get_meta, number, value
from .transcript_marks import META_KEY


def index_info(db):
    info = get_meta(db, META_KEY)
    if not info:
        raise ValueError('Import the native mark report into this vector corpus first.')
    return info


def metadata(db):
    info = index_info(db)
    def choices(column):
        return [r[0] for r in db.execute(f'SELECT DISTINCT {column} FROM native_marks WHERE {column} IS NOT NULL ORDER BY 1')]
    return dict(pages=info['pages'], masks=info['masks'], counts=info['counts'],
                expected=choices('expected_type'), predicted=choices('classifier_type'), bases=choices('base_character'))


def search(db, params):
    info = index_info(db)
    limit = number(params,'limit',36,True,1,120)
    offset = number(params,'offset',0,True,0,2_000_000)
    conditions, args = [], []
    def equals(column, val):
        conditions.append(column+'=?')
        args.append(val)
    for name, col in [('expected','expected_type'),('predicted','classifier_type'),('base','base_character')]:
        val = value(params,name)
        if val: equals('m.'+col,val)
    page = number(params,'page',None,True,1,604)
    if page: equals('m.page_id',f'page{page:03d}')
    q = value(params,'q').strip()
    if len(q)>256:
        raise ValueError('Search text is too long')
    exact_character = False
    if q:
        address = re.fullmatch(r'(?:(page\d{1,3}):)?(l\d+-w\d+)(-s\d+|-p\d+(?:[-:][\w-]+)?|:(?:c|sign)\d+(?::[\w-]+)?)?',q)
        if re.fullmatch(r'page\d{1,3}',q):
            equals('m.page_id',f'page{int(q[4:]):03d}')
        elif address:
            pid, word, suffix = address.groups()
            prefix = f'page{int(pid[4:]):03d}:' if pid else (f'page{page:03d}:' if page else '')
            column = 'm.word_id'
            local = word
            if suffix and suffix.startswith('-p'):
                # A split path contributes to each of its named components. Include
                # those children without treating p10 as a descendant of p1.
                path = json.dumps(word+suffix)
                conditions.append('(instr(m.path_ids,?)>0 OR instr(m.path_ids,?)>0)')
                args.extend((path,path[:-1]+'-'))
            elif suffix:
                local += suffix
                if suffix.startswith('-s'): column = 'm.subword_id'
                elif suffix.count(':')>1: column = 'm.id'
                elif suffix.startswith(':sign'): column = 'm.owner_id'
                else: column = 'm.character_id'
                exact_character = column in ('m.character_id','m.id')
            if prefix:
                equals(column,prefix+local)
            else:
                conditions.append(column+' LIKE ?')
                args.append('%:'+local)
        elif re.fullmatch(r'U\+[0-9A-Fa-f]{4,6}',q,re.IGNORECASE):
            equals('m.codepoint',q.upper())
        elif len(q)==1 and unicodedata.category(q).startswith('M'):
            equals('m.source_character',q)
        elif re.fullmatch(r'[a-z]+(?:[ -][a-z]+)*',q,re.IGNORECASE):
            name = q.casefold().replace(' ','-')
            conditions.append('(m.expected_type=? OR m.classifier_type=?)')
            args.extend((name,name))
        else:
            needle = plain(q).replace('\\','\\\\').replace('%','\\%').replace('_','\\_')
            conditions.append("w.bases LIKE ? ESCAPE '\\'")
            args.append('%'+needle+'%')
    verdict = value(params,'verdict')
    if not verdict:
        if not exact_character:  # A direct lookup can also identify a character without separate ink.
            conditions.append("m.verdict!='missing'")
    elif verdict == 'issues':
        conditions.append("m.verdict IN ('different','missing','ambiguous','unmatched')")
    elif verdict in ('match','accepted-form','different','missing','ambiguous','unmatched'):
        equals('m.verdict',verdict)
    else:
        raise ValueError('Unknown validation filter')
    source = ' FROM native_marks m JOIN words w ON w.id=m.word_id WHERE '+' AND '.join(conditions)
    total = info['masks'] if not q and not page and not verdict and len(conditions)==1 else db.execute(
        'SELECT COUNT(*)'+source,args).fetchone()[0]
    columns = ('m.id,m.page_id,m.word_id,m.subword_id,m.owner_id,m.base_character,m.base_offset,'
               'm.character_id,m.word_offset,m.source_character,m.codepoint,m.expected_type,m.equivalence,m.method,'
               'm.verdict,m.classifier_type,m.raw_type,m.classifier_kind,m.score,m.margin,m.candidates,m.path_ids,w.text')
    rows = [dict(r) for r in db.execute('SELECT '+columns+source+' ORDER BY m.seq LIMIT ? OFFSET ?',args+[limit,offset])]
    for row in rows:
        row['path_ids'] = json.loads(row['path_ids'] or '[]')
        row['candidates'] = json.loads(row['candidates'] or '[]')
    return dict(total=total, rows=rows, offset=offset, limit=limit)


@lru_cache(maxsize=4)
def page_vectors(blob):
    return json.loads(zlib.decompress(blob))


def image(db, identifier, context):
    row = db.execute('''SELECT m.x,m.y,m.w,m.h,m.runs,m.verdict,m.word_id,m.page_id,m.path_ids,p.width
        FROM native_marks m JOIN pages p ON p.page_id=m.page_id WHERE m.id=?''',(identifier,)).fetchone()
    if row is None:
        raise KeyError('Unknown mark')
    if context or row['verdict']=='missing':
        word = db.execute('SELECT x,y,w,h,runs FROM native_mark_words WHERE word_id=?',(row['word_id'],)).fetchone()
    else:
        word = None
    box = word or row
    pad = 5
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{box["x"]-pad} {box["y"]-pad} '
           f'{box["w"]+2*pad} {box["h"]+2*pad}" role="img"><title>{escape(identifier)}</title>']
    vectors = None
    if row['runs']==b'' or (word is not None and word['runs']==b''):
        art = db.execute('''SELECT a.vectors FROM catalog_page_art a JOIN pages p USING(page_id)
            JOIN native_mark_pages n USING(page_id)
            WHERE a.page_id=? AND a.source_sha=p.source_sha AND a.report_sha=n.report_sha''',(row['page_id'],)).fetchone()
        if art:
            vectors = page_vectors(art[0])
    for item, color in ((word,'#b5beb7'),(row,'#165c49' if word else '#223e32')):
        if item is not None and item['runs']:
            svg.append(f'<path fill="{color}" d="{mask_path(unpack_runs(item["runs"]),row["width"])}"/>')
        elif item is not None and item['runs']==b'' and vectors:
            paths = vectors['paths']
            if item is word:
                prefix = row['word_id'].split(':',1)[1]+'-p'
                outlines = [path for name,path in paths.items()
                            if name.startswith(prefix) and name[len(prefix):].isdigit()]
            else:
                outlines = [paths.get(i) for i in json.loads(row['path_ids'] or '[]')]
                if identifier in vectors.get('rasterMarks',[]) or not all(outlines):
                    outlines = []
            for outline in outlines:
                d = escape(outline['d'],quote=True)
                rule = escape(outline.get('fillRule','nonzero'),quote=True)
                svg.append(f'<path fill="{color}" fill-rule="{rule}" d="{d}"/>')
    return ''.join(svg)+'</svg>'


def report_file(db, name):
    page = re.fullmatch(r'(page\d{3})\.html',name)
    if name!='index.html' and (not page or not db.execute('SELECT 1 FROM native_mark_pages WHERE page_id=?',(page[1],)).fetchone()):
        raise ValueError('Unknown report page')
    return (Path(index_info(db)['reportDirectory'])/name).read_bytes()
