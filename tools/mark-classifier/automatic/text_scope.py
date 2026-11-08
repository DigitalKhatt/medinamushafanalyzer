"""Identify non-text regions using the source SVG and the edition's line map."""
from workbench_paths import NATIVE
from pathlib import Path
import re
from .common import plain

BASMALA = ('بسم', 'الله', 'الرحمن', 'الرحيم')


def basmala_lines():
    source = NATIVE/'quran.cpp'
    pages = re.findall(r'R"page(\d+)\(\n(.*?)\n\)page\1"', source.read_text(), re.S)
    if len(pages) != 604:
        raise ValueError('Missing edition transcript lines')
    return {
        f'page{int(page)+1:03}': tuple(
            line for line, text in enumerate(body.splitlines(), 1)
            if tuple(plain(text).split()[:4]) == BASMALA
            and line>1 and body.splitlines()[line-2].startswith('سُورَةُ')
        )
        for page, body in pages
    }


def text_exclusions(geometry, rows, words, openings):
    vx, vy, vw, vh = geometry['viewBox']
    sx, sy = geometry['width']/vw, geometry['height']/vh

    def raster_box(box):
        x, y, w, h = box
        return (x-vx)*sx, (y-vy)*sy, w*sx, h*sy

    markers = [raster_box(marker['bbox']) for marker in geometry.get('verseMarkers', [])]
    result = {}
    by_line = {}
    for word in words:
        by_line.setdefault(word['line'], []).append(word)
    numeric = {word['id'] for word in words if word['bases'].isdecimal()}
    for row in rows:
        x, y, w, h = (row[k] for k in ('x','y','w','h'))
        cx, cy = x+w/2, y+h/2
        if row['teacher_type']=='verse-number' or row['word_id'] in numeric or any(
            mx+mw*.12<=cx<=mx+mw*.88 and my+mh*.12<=cy<=my+mh*.88
            and w<=mw and h<=mh for mx,my,mw,mh in markers
        ):
            result[row['id']] = 'verse-marker'

    for line in openings:
        linked = {word['id'] for word in by_line.get(line, []) if word['bases'] in BASMALA}
        if linked:
            if len(linked)!=len(BASMALA):
                raise ValueError('Incomplete linked basmala')
            for row in rows:
                if row['word_id'] in linked:
                    result[row['id']] = 'basmala'
            continue
        next_words = by_line[min(number for number in by_line if number>line)]
        previous = [word for word in words if word['line']<line]
        top = max((word['y']+word['h'] for word in previous), default=0)
        bottom = min(word['y'] for word in next_words)
        titles = [raster_box(title['bbox']) for title in geometry.get('surahFrames', [])]
        top = max([top]+[y+h for _,y,_,h in titles if top<y+h<bottom])
        for row in rows:
            if row['word_id'] is None and top<=row['y'] and row['y']+row['h']<=bottom:
                result.setdefault(row['id'], 'basmala')
    return result
