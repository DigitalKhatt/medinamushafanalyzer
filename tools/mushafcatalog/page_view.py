"""Rebuild one independently colorable Mushaf page from SQLite."""
from collections import defaultdict
from dataclasses import dataclass
from functools import lru_cache
from html import escape
import json
import re
import unicodedata
import zlib

from page_join import choose_boundary, half_plane, outline_edges
from store import ink_hash

INK = '#231f20'
PALETTE = ('#003479','#e64d00','#5c008a','#00975a','#85002d','#0083c4')
KASHIDA_COLOR = '#52627b'
DETACHED_LETTER_COLORS = {
    'hamza-above': '#c9008b',
    'small-alef': '#007f9e',
    'small-high-yeh': '#b74700',
    'small-meem': '#6045ce',
    'small-noon': '#007568',
}
DOT_LABELS = {'single-dot':'Dot','double-dot':'Two dots','triple-dot':'Three dots'}


@dataclass
class DetachedLetter:
    id: str
    offset: int
    character: str
    text: str
    color: str
    status: str
    x: float | None
    bottom: int | None


def character_attributes(character):
    if not character:
        return ''
    name = unicodedata.name(character,'') if len(character)==1 else ''
    return (f' data-character="{escape(character,quote=True)}"'
            f' data-character-name="{escape(name,quote=True)}"')


@lru_cache(maxsize=8)
def decode_vectors(blob):
    # A republished cut changes the cache key even if the source hashes agree.
    return json.loads(zlib.decompress(blob)) if blob else dict(paths={},parts={})


def matching_part(vectors, owner, role, blob):
    outline = vectors['parts'].get(owner+'/'+role)
    return outline if outline and outline['ink'] == ink_hash(blob) else None


def mark_outlines(vectors, raster_marks, mark, ids):
    if mark in raster_marks:
        return []
    paths = [vectors['paths'].get(i) for i in json.loads(ids or '[]')]
    return paths if all(paths) else []


def painted_outline(outline):
    fill = outline.get('fill', INK)
    opacity = float(outline.get('opacity', 1))
    rule = outline['fillRule']
    return (f'<path d="{escape(outline["d"],quote=True)}" fill="{escape(fill,quote=True)}" '
            f'fill-rule="{escape(rule,quote=True)}" opacity="{opacity:g}"/>')


def page_svg(db, page, colored=False, *, interior=False, kashida='middle'):
    if kashida not in ('middle','left','right','standalone'):
        raise ValueError('Unknown kashida display mode')
    if not re.fullmatch(r'(?:page)?[0-9]{1,3}', str(page)):
        raise ValueError('Choose a page from 1 to 604')
    pid = f"page{int(str(page).removeprefix('page')):03}"
    row = db.execute('''SELECT width,height,source_sha,report_sha FROM pages
        JOIN catalog_pages USING(page_id) WHERE page_id=?''',(pid,)).fetchone()
    if row is None:
        raise KeyError(f'{pid} is outside this catalog')
    width,height,source_hash,report_hash = row
    art = None
    if db.execute("SELECT 1 FROM sqlite_master WHERE name='catalog_page_art'").fetchone():
        art = db.execute('''SELECT vectors FROM catalog_page_art
            WHERE page_id=? AND source_sha=? AND report_sha=?''',(pid,source_hash,report_hash)).fetchone()
    if not art:
        raise KeyError(f'{pid}: original vector artwork is unavailable. Run the native letter cutter for this page.')
    vectors = decode_vectors(art[0])
    native_art = vectors.get('pageArt')
    if not native_art:
        raise KeyError(f'{pid}: original vector artwork is unavailable. Run the native letter cutter for this page.')
    defs, ink, clips, uses = [], [], [], defaultdict(list)

    def shape(outline):
        d = outline['d']
        rule = outline.get('fillRule','nonzero')
        name = 'p'+str(len(defs))
        defs.append(f'<path id="{name}" d="{escape(d,quote=True)}" fill-rule="{rule}"/>')
        ref = f'<use href="#{name}"'
        ink.append(ref+'/>')
        return ref

    owners = list(db.execute('''SELECT o.id,o.character,o.text,o.word_id,m.quality
        FROM catalog_subwords s JOIN words w ON w.id=s.word_id
        JOIN catalog_owners o ON o.subword_id=s.id
        LEFT JOIN catalog_measurements m ON m.owner_id=o.id WHERE s.page_id=? AND o.role='letter'
        ORDER BY w.line,w.ordinal,o.word_offset''',(pid,)))
    owner_ids = {o['id'] for o in owners}
    tatweel_owners = {o['id'] for o in owners if o['character']=='ـ'}
    bodies, joins = set(), []
    tatweel_strokes = {}
    by_subword = defaultdict(list)
    parts = db.execute('''SELECT s.id,p.owner_id,p.role,p.next_owner_id,p.runs,p.x,p.y,p.w FROM catalog_subwords s
        JOIN catalog_parts p ON p.subword_id=s.id WHERE s.page_id=? ORDER BY s.seq,p.owner_id,p.role''',(pid,))
    for sid,owner,role,neighbor,blob,x,y,w in parts:
        outline = matching_part(vectors,owner,role,blob)
        by_subword[sid].append((owner,role,neighbor,outline,(x,y,w)))
    for parts in by_subword.values():
        # Keep the original subword intact when a complete vector partition
        # cannot be verified against the current assignment.
        if any(not outline or (role!='body' and not outline.get('caps'))
               for _,role,_,outline,_ in parts):
            continue
        for owner,role,neighbor,outline,bounds in parts:
            ref = shape(outline)
            if role=='body':
                bodies.add(owner)
                if owner in tatweel_owners:
                    tatweel_strokes[owner] = ref
                else:
                    uses[owner].append(ref+' data-part="body"/>')
            else:
                joins.append((owner,neighbor,outline,ref,bounds))

    raster_marks = set(vectors.get('rasterMarks', []))
    pending_marks = 0
    waqf_by_word = defaultdict(list)
    detached_letters = defaultdict(list)
    detached_offsets = defaultdict(set)
    for owner,mark,ids,expected,text,role,word,character,carrier,character_id,offset,x,y,w,h in db.execute('''
                SELECT m.owner_id,m.id,m.path_ids,m.expected_type,o.text,o.role,o.word_id,
                    m.source_character,o.character,m.character_id,m.word_offset-o.word_offset,m.x,m.y,m.w,m.h
                FROM native_marks m JOIN catalog_owners o ON o.id=m.owner_id
                WHERE m.page_id=? AND m.runs IS NOT NULL ORDER BY m.owner_id,m.word_offset,m.seq''',(pid,)):
        is_waqf = role=='word-sign' and (expected or '').startswith('waqf-')
        if owner not in owner_ids and not is_waqf: continue
        paths = mark_outlines(vectors,raster_marks,mark,ids)
        if not paths:
            pending_marks += 1
            continue
        attributes = character_attributes(character)
        color = DETACHED_LETTER_COLORS.get(expected)
        separate = carrier=='ـ'
        if expected=='small-alef':
            separate = offset is not None and offset>0 and text[offset-1:offset+1]=='َٰ'
        if color and character_id and character and offset is not None and separate:
            status = 'small alef after fatha' if expected=='small-alef' else 'tatweel letter'
            glyph = DetachedLetter(character_id,offset,character,character,color,status,
                                   x+w/2 if x is not None else None,y+h if y is not None else None)
            detached_letters[owner].append(glyph)
            detached_offsets[owner].add(offset)
            uses[glyph.id].extend(shape(path)+' data-part="body"/>' for path in paths)
            continue
        if expected in DOT_LABELS and character and len(character)==1:
            letter_name = unicodedata.name(character,'').removeprefix('ARABIC LETTER ').lower()
            if letter_name:
                attributes += f' data-dot-label="{escape(DOT_LABELS[expected]+" of "+letter_name,quote=True)}"'
        marks = [shape(path)+f' data-part="mark" data-mark="{escape(mark,quote=True)}"{attributes}/>' for path in paths]
        detached = detached_letters[owner]
        if is_waqf:
            waqf_by_word[word].append(f'<g class="waqf" data-owner="{escape(owner,quote=True)}" '
                                     f'data-text="{escape(text,quote=True)}" data-quality="waqf" fill="{INK}"{attributes}>'
                                     f'<title>{escape(text)} · {escape(owner)}</title>'+''.join(marks)+'</g>')
        elif detached and character_id and character and offset is not None and offset>detached[-1].offset:
            glyph = detached[-1]
            glyph.text += character
            detached_offsets[owner].add(offset)
            uses[glyph.id].extend(marks)
        else:
            uses[owner].extend(marks)

    def color_stroke(ref, glyphs):
        # Two letters over one stroke share it at the midpoint between them.
        glyphs = sorted(glyphs,key=lambda glyph: glyph.x if glyph.x is not None else width/2)
        centers = [glyph.x if glyph.x is not None else width/2 for glyph in glyphs]
        cuts = [0, *((a+b)/2 for a,b in zip(centers,centers[1:])), width]
        for glyph,left,right in zip(glyphs,cuts,cuts[1:]):
            clip_attr = ''
            if len(glyphs)>1:
                clip = f'glyph-stroke-{len(clips)}'
                clips.append(f'<clipPath id="{clip}"><rect x="{left:g}" width="{right-left:g}" height="{height}"/></clipPath>')
                clip_attr = f' clip-path="url(#{clip})"'
            uses[glyph.id].append(ref+f'{clip_attr} data-part="kashida"/>')

    for owner,ref in tatweel_strokes.items():
        if detached_letters[owner]:
            color_stroke(ref,detached_letters[owner])
        else:
            uses[owner].append(ref+' data-part="body"/>')

    # The transcript decides which glyphs detach. Bounds only identify the
    # neighboring connector that should take the detached glyph's color.
    nearest_join = {}
    for i,(owner,neighbor,_,_,(x,y,w)) in enumerate(joins):
        for letter in (owner,neighbor):
            if letter in tatweel_strokes:
                continue
            for glyph in detached_letters[letter]:
                if glyph.x is None or glyph.bottom is None or not x<=glyph.x<=x+w or glyph.bottom>y:
                    continue
                distance = (y-glyph.bottom,abs(x+w/2-glyph.x))
                previous = nearest_join.get(glyph.id)
                if previous is None or distance<previous[0]:
                    nearest_join[glyph.id] = (distance,i,glyph)
    colored_joins = defaultdict(list)
    for _,i,glyph in nearest_join.values():
        colored_joins[i].append(glyph)

    standalone = []
    for i,(owner,neighbor,outline,ref,_) in enumerate(joins):
        if i in colored_joins:
            color_stroke(ref,colored_joins[i])
            continue
        if kashida=='standalone':
            address = f'{owner} → {neighbor}'
            standalone.append(f'<g class="kashida" data-text="Kashida" data-owner="{escape(address,quote=True)}" '
                              f'data-palette="{KASHIDA_COLOR}" fill="{KASHIDA_COLOR if colored else INK}">'
                              f'<title>Kashida · {escape(address)}</title>{ref} data-part="kashida"/></g>')
            continue
        if kashida!='middle':
            letter = neighbor if kashida=='left' else owner
            uses[letter].append(ref+' data-part="kashida"/>')
            continue
        point,axis,method = choose_boundary(outline['caps'],outline_edges(outline['d']))
        for side,letter,sign in (('a',owner,-1),('b',neighbor,1)):
            clip = f'j{i}{side}'
            poly = half_plane(point,(axis[0]*sign,axis[1]*sign),width,height)
            clips.append(f'<clipPath id="{clip}"><polygon points="{poly}"/></clipPath>')
            uses[letter].append(ref+f' clip-path="url(#{clip})" data-part="join-half" data-split="{method}"/>')

    ornaments = []
    view_box = f'0 0 {width} {height}'
    skipped = {native_art['frame']} if interior else set()
    for glyph in (*native_art['headings'], *native_art['ayahs']):
        kind = escape(glyph['kind'],quote=True)
        text = escape(glyph['text'],quote=True)
        shapes = []
        if 'marker' in glyph:
            skipped.add(glyph['marker'])
            shapes.append(painted_outline(native_art['paint'][glyph['marker']]))
        for path in glyph['paths']:
            shapes.append(shape(path)+'/>')
        ornaments.append(f'<g data-decoration="{kind}" data-line="{glyph["line"]}" '
                         f'aria-label="{text}" fill="{INK}">'+''.join(shapes)+'</g>')
    background = ''.join(painted_outline(path) for i,path in enumerate(native_art['paint']) if i not in skipped)
    if interior:
        opening = native_art['opening']
        view_box = ' '.join(f'{value:.9g}' for value in opening['bounds'])
        clips.append(f'<clipPath id="frame-opening"><path d="{escape(opening["d"],quote=True)}" '
                     f'clip-rule="evenodd"/></clipPath>')
    # Knock out the owned ink so a letter can be moved or hidden independently.
    defs.append(f'<mask id="paper" maskUnits="userSpaceOnUse" x="0" y="0" width="{width}" height="{height}">'
                f'<rect width="{width}" height="{height}" fill="white"/><g fill="black">'+''.join(ink)+'</g></mask>')
    groups, by_word = [], defaultdict(list)
    for o in owners: by_word[o['word_id']].append(o)
    for word in waqf_by_word: by_word.setdefault(word,[])
    for word,letters in by_word.items():
        groups.append(f'<g class="word" data-word="{escape(word)}">')
        for index,o in enumerate(letters):
            owner = o['id']
            color = PALETTE[index%len(PALETTE)]
            status = (o['quality'] or 'segmented') if owner in bodies else 'pending'
            label = ''.join(char for offset,char in enumerate(o['text']) if offset not in detached_offsets[owner])
            groups.append(f'<g class="letter" data-owner="{escape(owner)}" data-index="{index}" data-text="{escape(label,quote=True)}" '
                          f'data-quality="{escape(status)}" data-palette="{color}" fill="{color if colored else INK}"'
                          f'{character_attributes(o["character"])}>'
                          f'<title>{escape(label)} · {escape(owner)}</title>'+''.join(uses[owner])+'</g>')
            for glyph in detached_letters[owner]:
                glyph_id = escape(glyph.id,quote=True)
                label = escape(glyph.text,quote=True)
                groups.append(f'<g class="letter" data-owner="{glyph_id}" data-text="{label}" data-quality="{glyph.status}" '
                              f'data-palette="{glyph.color}" fill="{glyph.color if colored else INK}"'
                              f'{character_attributes(glyph.character)}><title>{label} · {glyph_id}</title>'
                              + ''.join(uses[glyph.id]) + '</g>')
        groups.extend(waqf_by_word[word])
        groups.append('</g>')
    pending = len(owners) - len(bodies)
    content = f'<g fill="{INK}" pointer-events="none" mask="url(#paper)">{background}</g>'+''.join(ornaments+groups+standalone)
    if interior:
        content = '<g clip-path="url(#frame-opening)">'+content+'</g>'
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{view_box}" data-page="{pid}" '
            f'data-letters="{len(owners)}" data-pending="{pending}" data-pending-marks="{pending_marks}" data-artwork="1">'
            f'<title>1441 Hafs · {pid}</title><defs>'+''.join(defs+clips)+'</defs>'
            +content+'</svg>')
