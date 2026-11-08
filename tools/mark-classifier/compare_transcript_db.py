#!/usr/bin/env python3
"""Compare all native analyzer pages with qpc_v4_layout / words.dk_v2, read-only."""
import argparse
from collections import Counter, defaultdict
from contextlib import closing
from difflib import SequenceMatcher
from html import escape
import json
from pathlib import Path
import sqlite3
import unicodedata

from workbench_paths import DATA, NATIVE
from automatic.common import digest_file
from automatic.native_transcript import validate_snapshot


def key(value):
    # Preserve all letters and vowel signs. Only canonical Unicode equivalence
    # and the independently reported verse/hizb wrappers guide word alignment.
    return unicodedata.normalize('NFC', value.replace('\u06dd', '').replace('۞', ''))


def word_ref(word, column):
    return dict(id=word['id'], line=word['line'], ordinal=word['ordinal'], text=word[column],
                **({k: word[k] for k in ('word_number_all', 'surah_number', 'ayah_number', 'word_number')}
                   if column == 'dk_v2' else {}))


def compare_page(native, database):
    a = [w for w in native if key(w['text'])]
    b = [w for w in database if key(w['dk_v2'])]
    result = dict(nativeEntries=len(native), databaseEntries=len(database),
                  nativeStandaloneHizb=sum(w['text'] == '۞' for w in native),
                  canonicalUnicode=[], verseMarkerPrefix=[], hizbWordAssociation=[],
                  spellingDifferences=[], wordBoundaries=[], lineBoundaries=[], otherDifferences=[])
    matcher = SequenceMatcher(None, [key(w['text']) for w in a], [key(w['dk_v2']) for w in b], autojunk=False)
    for tag, i, j, k, l in matcher.get_opcodes():
        if tag == 'equal':
            for x, y in zip(a[i:j], b[k:l]):
                case = dict(native=[word_ref(x, 'text')], database=[word_ref(y, 'dk_v2')])
                if x['line'] != y['line']:
                    result['lineBoundaries'].append(case)
                if x['text'] == y['dk_v2']:
                    continue
                if unicodedata.normalize('NFC', x['text']) == unicodedata.normalize('NFC', y['dk_v2']):
                    result['canonicalUnicode'].append(case)
                if '\u06dd' in y['dk_v2'] and '\u06dd' not in x['text']:
                    result['verseMarkerPrefix'].append(case)
                if x['text'].count('۞') != y['dk_v2'].count('۞'):
                    result['hizbWordAssociation'].append(case)
        else:
            x = ''.join(key(w['text']) for w in a[i:j])
            y = ''.join(key(w['dk_v2']) for w in b[k:l])
            case = dict(native=[word_ref(w, 'text') for w in a[i:j]],
                        database=[word_ref(w, 'dk_v2') for w in b[k:l]])
            if x.replace(' ', '') == y.replace(' ', ''):
                result['wordBoundaries'].append(case)
            elif key(x.replace('أٓ', 'ـَٔا')) == y:
                # Record this source spelling difference; never normalize it
                # away when comparing mark inventories or alter either input.
                result['spellingDifferences'].append(case)
            else:
                result['otherDifferences'].append(case)
    return result


def compare(root, output, database=None, snapshot=None):
    root, output = Path(root).resolve(), Path(output).resolve()
    database = Path(database).resolve() if database else root/'visualmetafont/src/Resources/quran-data.sqlite'
    snapshot = Path(snapshot).resolve() if snapshot else DATA/'automatic/word-audit/native-transcript.json'
    native = validate_snapshot(json.loads(snapshot.read_text()), (root/'medinamushafanalyzer/quran.cpp').read_text())
    layouts = defaultdict(list)
    db_words = defaultdict(list)
    with closing(sqlite3.connect(database.as_uri()+'?mode=ro', uri=True)) as db:
        db.row_factory = sqlite3.Row
        db.execute('BEGIN')
        words = {r['word_number_all']: dict(r) for r in db.execute(
            'SELECT word_number_all,dk_v2,is_ayah_marker,surah_number,ayah_number,word_number FROM words')}
        for row in db.execute('SELECT * FROM qpc_v4_layout ORDER BY page,line'):
            pid = f"page{row['page']:03}"
            layouts[pid].append(dict(row))
            if row['type'] != 'ayah':
                continue
            for ordinal, wid in enumerate(range(row['range_start'], row['range_end']+1), 1):
                db_words[pid].append(dict(words[wid], id=f"{pid}:l{row['line']}-w{ordinal}",
                                          line=row['line'], ordinal=ordinal))
    if set(native) != set(layouts):
        raise ValueError('Native and database layout pages differ')
    pages = {}
    for pid, entries in native.items():
        ordered = list(entries.values())
        result = compare_page(ordered, db_words[pid])
        result['layout'] = layouts[pid]
        native_counts = Counter(w['line'] for w in ordered)
        database_counts = Counter(w['line'] for w in db_words[pid])
        result['lineEntryCounts'] = [dict(line=line, native=native_counts[line], database=database_counts[line])
                                     for line in sorted(set(native_counts) | set(database_counts))]
        pages[pid] = result
    # Match hizb occurrences in text order and retain both attachment conventions.
    signs = [[], []]
    for pid, entries in native.items():
        for w in entries.values():
            signs[0].extend(dict(page=pid, **word_ref(w, 'text')) for _ in range(w['text'].count('۞')))
        for w in db_words[pid]:
            signs[1].extend(dict(page=pid, **word_ref(w, 'dk_v2')) for _ in range(w['dk_v2'].count('۞')))
    hizb = [dict(native=a, database=b) for a, b in zip(*signs)] if len(signs[0]) == len(signs[1]) else []
    categories = ('canonicalUnicode', 'verseMarkerPrefix', 'hizbWordAssociation', 'spellingDifferences',
                  'wordBoundaries', 'lineBoundaries', 'otherDifferences')
    summary = dict(pages=len(pages), nativeEntries=sum(p['nativeEntries'] for p in pages.values()),
        databaseEntries=sum(p['databaseEntries'] for p in pages.values()),
        counts={c: sum(len(p[c]) for p in pages.values()) for c in categories},
        pagesByCategory={c: sum(bool(p[c]) for p in pages.values()) for c in categories},
        nativeStandaloneHizb=sum(p['nativeStandaloneHizb'] for p in pages.values()),
        nativeHizbSigns=len(signs[0]), databaseHizbSigns=len(signs[1]),
        hizbPageLineDifferences=sum((h['native']['page'], h['native']['line']) !=
                                   (h['database']['page'], h['database']['line']) for h in hizb),
        pagesWithEntryCountDifferences=sum(p['nativeEntries'] != p['databaseEntries'] for p in pages.values()),
        pagesWithLineEntryCountDifferences=sum(any(r['native'] != r['database'] for r in p['lineEntryCounts']) for p in pages.values()),
        comparison=dict(layout='qpc_v4_layout', textColumn='dk_v2', database=str(database),
                        databaseSha256=digest_file(database), nativeSnapshot=str(snapshot),
                        nativeSnapshotSha256=digest_file(snapshot), nativeSource=str(root/'medinamushafanalyzer/quran.cpp'),
                        nativeSourceSha256=digest_file(root/'medinamushafanalyzer/quran.cpp')),
        policy='Read-only transcript comparison. No database, analyzer source, segmentation or labels changed. Categories can overlap. Surah-name and basmallah layout rows are retained as layout metadata and excluded from word comparison, as in the native initialized transcript.')
    output.mkdir(parents=True, exist_ok=True)
    document = dict(summary=summary, pages=pages, hizbOccurrences=hizb)
    (output/'differences.json').write_text(json.dumps(document, ensure_ascii=False, indent=2))
    (output/'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2))
    # Focused report: retain encoding-only cases in JSON, show all text and
    # boundary differences in the human-readable page list.
    parts = ['<!doctype html><html lang="en"><meta charset="utf-8"><title>dk_v2 transcript comparison</title>',
             '<style>body{font:16px system-ui;margin:32px;max-width:1400px;color:#172636}table{border-collapse:collapse;width:100%;margin:20px 0}td,th{border:1px solid #ccd4dc;padding:10px;text-align:left}td:nth-child(n+3){font-size:23px}code{font-size:13px}h2{margin-top:40px}</style>',
             '<h1>All 604 pages · qpc_v4_layout / dk_v2</h1>',
             '<p>Compared with the current native medinamushafanalyzer transcript. Sources are unchanged.</p>',
             '<pre>'+escape(json.dumps({k:v for k,v in summary.items() if k not in ('comparison','policy')}, ensure_ascii=False, indent=2))+'</pre>']
    for category, label in [('spellingDifferences','Hamza / maddah versus fatha / alef spelling'),
                            ('wordBoundaries','Word tokenization'), ('lineBoundaries','Line placement'),
                            ('otherDifferences','Other text or mark differences')]:
        parts.extend([f'<h2>{escape(label)}</h2>', '<table><tr><th>Page</th><th>Native location → database location</th><th>Analyzer</th><th>dk_v2</th></tr>'])
        for pid, page in pages.items():
            for case in page[category]:
                loc=lambda rows: ', '.join(r['id'].split(':',1)[1] for r in rows)
                texts=lambda rows: ' | '.join(r['text'] for r in rows)
                parts.append('<tr>'+''.join('<td>'+escape(v)+'</td>' for v in
                    (pid,loc(case['native'])+' → '+loc(case['database']),texts(case['native']),texts(case['database'])))+'</tr>')
        parts.append('</table>')
    parts.append('<p>All canonical Unicode, verse-marker, hizb attachment and layout records: <a href="differences.json">differences.json</a>.</p></html>')
    (output/'index.html').write_text('\n'.join(parts))
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--database', type=Path)
    parser.add_argument('--snapshot', type=Path)
    parser.add_argument('--output', type=Path, default=DATA/'automatic/transcript-comparison-dk-v2')
    args = parser.parse_args()
    print(json.dumps(compare(NATIVE.parent, args.output, args.database, args.snapshot), ensure_ascii=False, indent=2))
