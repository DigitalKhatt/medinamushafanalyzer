"""Join transcript ownership, vector segmentation and visual mark recognition.

Read-only diagnostics: inventories constrain ownership, but repeated mark types
cannot prove which physical occurrence belongs to which word.
"""
from collections import Counter, defaultdict
from contextlib import closing
from functools import lru_cache
import csv
import hashlib
from html import escape
import json
from pathlib import Path
import re
import sqlite3
import zlib

from .common import digest_file, unpack_runs, input_root
from .propose import teacher_module
from .native_transcript import export_transcript, subword_characters

BODY = {'subword-ink', 'body', 'kashida'}
SEMANTIC = {'dot', 'mark', 'symbol'}
SOURCES = {'final': ('final_kind', 'final_type'),
           'model': ('prediction_kind', 'prediction_type'),
           'proposal': ('teacher_kind', 'teacher_type')}
_CACHED_CHARACTERS = object()


@lru_cache(maxsize=1)
def teacher():
    return teacher_module()


def expected_inventory(characters):
    # Use precisely the edition's joining-position/dot/Unicode conventions.
    result = Counter(s['type'] for s in teacher().slots({'characters': characters})
                     if s['type'] != 'small-kaf-body')
    result['hizb-sign'] += sum(c['text'] == '۞' for c in characters)
    result['quranic-sign'] += sum(c['text'] == '۩' for c in characters)
    return +result


def observed_inventory(items, source='final'):
    role, subtype = SOURCES[source]
    return Counter(i[subtype] or 'unknown' for i in items if i[role] in SEMANTIC)


def inventory_check(expected, items, source='final'):
    observed = observed_inventory(items, source)
    if expected is None:
        return dict(expected=None, observed=dict(observed), missing=None, extra=None,
                    matches=False, checked=False)
    expected = Counter(expected)
    return dict(expected=dict(expected), observed=dict(observed),
                missing=dict(expected-observed), extra=dict(observed-expected),
                matches=expected == observed)


def check_subword(sub, items, characters=_CACHED_CHARACTERS):
    if characters is _CACHED_CHARACTERS:
        characters = json.loads(sub['expected'])
    expected = expected_inventory(characters) if characters is not None else None
    checks = {s: inventory_check(expected, items, s) for s in SOURCES}
    issues = []
    if not checks['final']['matches']:
        issues.append('mark-inventory')
    if not any(i['final_kind'] in BODY for i in items):
        issues.append('missing-body')
    if any(i['final_kind'] in (None, 'unknown') for i in items):
        issues.append('unknown-ink')
    if any(i['final_kind'] in SEMANTIC and not i['human_reviewed'] and
           (i['model_confidence'] is None or i['model_confidence'] < .9) for i in items):
        issues.append('low-mark-score')
    if any(i['prediction_guard'] for i in items):
        issues.append('structural-warning')
    return dict(id=sub['id'], text=sub['text'], inventories=checks,
                issues=issues, bodyItems=[i['id'] for i in items if i['final_kind'] in BODY],
                itemIds=[i['id'] for i in items],
                attachmentReviewed=False)  # region review does not approve ownership


def ownership_hints(records, items, same_word=False):
    """Possible transfers only; never pick arbitrarily among identical marks."""
    output = []
    for target in records:
        missing = target['inventories']['final']['missing'] or {}
        for name, count in missing.items():
            candidates = []
            for donor in records:
                if donor['id'] == target['id'] or donor['line'] != target['line']:
                    continue
                if same_word:
                    if donor['wordId'] != target['wordId']:
                        continue
                elif abs(donor['ordinal']-target['ordinal']) != 1:
                    continue
                extra = (donor['inventories']['final']['extra'] or {}).get(name, 0)
                if not extra:
                    continue
                candidates.append(dict(fromId=donor['id'], excess=extra,
                    itemIds=[i['id'] for i in items if
                             i['subword_id' if same_word else 'word_id'] == donor['id']
                             and i['final_kind'] in SEMANTIC and i['final_type'] == name]))
            if candidates:
                output.append(dict(toId=target['id'], type=name, missing=count,
                    candidates=candidates, status='ownership-proposal',
                    uniqueInventoryDonor=len(candidates) == 1,
                    note='Transcript deficit and neighboring surplus; classification and physical occurrence still need inspection.'))
    return output


def mask_union(items, size):
    """Validate runs, disjointness and exact pixels, retaining every tiny mark."""
    mask = bytearray(size)
    for item in items:
        runs = item['runs']
        if len(runs) % 2:
            raise ValueError('Odd RLE length')
        previous = -1
        area = 0
        for start, count in zip(runs[::2], runs[1::2]):
            if start < 0 or count <= 0 or start+count > size or start < previous:
                raise ValueError('Invalid or overlapping RLE')
            if any(mask[start:start+count]):
                raise ValueError('Overlapping source ink')
            mask[start:start+count] = b'\1'*count
            previous = start+count
            area += count
        if area != item['area']:
            raise ValueError('Region area differs from exact mask')
    return mask


def page_geometry(page, rows, payload):
    size = page['width']*page['height']
    current = mask_union(rows, size)
    frozen = mask_union(payload['items'], size)
    if current != frozen or sum(current) != page['ink_pixels']:
        raise ValueError('Corpus ink differs from frozen source union')
    return dict(exactSourceUnion=True, pixels=sum(current),
                maskSha256=hashlib.sha256(current).hexdigest())


def union_intervals(run_lists):
    result = []
    for start, end in sorted((s, s+n) for runs in run_lists
                             for s, n in zip(runs[::2], runs[1::2])):
        if result and start <= result[-1][1]:
            result[-1][1] = max(end, result[-1][1])
        else:
            result.append([start, end])
    return result


def alignment(items, path_runs):
    """Check each exact source mask against its vector owner, never just boxes."""
    from bisect import bisect_right
    intervals = union_intervals(path_runs)
    starts = [s for s, e in intervals]
    mismatches = []
    minimum = 1.
    for item in items:
        hits = 0
        for start, count in zip(item['runs'][::2], item['runs'][1::2]):
            stop = start+count
            k = max(0, bisect_right(starts, start)-1)
            while k < len(intervals) and intervals[k][0] < stop:
                a, b = intervals[k]
                hits += max(0, min(b, stop)-max(a, start))
                k += 1
        coverage = hits/max(1, item['area'])
        minimum = min(minimum, coverage)
        if coverage < .8:
            mismatches.append(dict(id=item['id'], sourceCoverage=coverage))
    return dict(minimumSourceCoverage=minimum if items else None,
                mismatches=mismatches, threshold=.8,
                policy='Exact source-mask overlap with vector owner; alignment evidence, not semantic ownership proof.')


def letter_status(cache, word, subwords, current_config, current_version, provenance):
    if cache is None:
        return {'status': 'not-computed'}
    stale = (cache.get('templateConfig') != current_config or
             cache.get('version') != current_version or
             cache.get('pageDataSha256') != provenance.get('cacheSha256') or
             cache.get('fontSha256') != provenance.get('templateSha256'))
    entries = [s for s in cache['subwords'] if s['line'] == word['line'] and s['word'] == word['ordinal']]
    if stale:
        return {'status': 'stale', 'subwords': len(entries)}
    # The native cache includes detached marks/signs normalized out by preparation.
    # Match by text/body bounds rather than treating its subword indices as universal.
    texts = Counter(s['text'] for s in entries)
    matched = not (Counter(s['text'] for s in subwords)-texts)
    return dict(status='complete' if entries and matched and all(s['complete'] for s in entries)
                else 'incomplete-or-normalized', subwords=len(entries),
                incomplete=sum(not s['complete'] for s in entries))


def audit_page(db, root, native, page, index, config, version, transcript):
    pid = page['page_id']
    proposal = json.loads((root/'proposals'/f'{pid}.json').read_text())
    payload = json.loads(zlib.decompress((root/'pages'/f'{pid}.json.zlib').read_bytes()))
    from review_bridge import proposal_digest
    errors = []
    if proposal_digest(proposal) != page['proposal_sha']:
        errors.append('proposal-snapshot-changed')
    source = Path(index['sourceDir'])/next(p['filename'] for p in index['pages'] if p['id'] == pid)
    if digest_file(source) != page['source_sha'] or proposal['sourceSha256'] != page['source_sha']:
        errors.append('source-changed')
    prov = proposal['provenance']
    if digest_file(native/'pages'/f'page{int(pid[4:])}.dat') != prov['cacheSha256']:
        errors.append('analyzer-word-cache-changed')
    if digest_file(native/'input/font.dat') != prov['templateSha256']:
        errors.append('analyzer-font-changed')
    data_root=input_root(root)
    annotation = data_root/'annotations'/f'{pid}.json'
    if annotation.exists() and json.loads(annotation.read_text())['revision'] != page['annotation_revision']:
        errors.append('human-annotation-newer-than-corpus')
    rows = [dict(r) for r in db.execute('SELECT * FROM items WHERE page_id=?', (pid,))]
    for row in rows:
        row['runs'] = unpack_runs(row['runs'])
    try:
        geometry = page_geometry(page, rows, payload)
    except ValueError as error:
        geometry = dict(exactSourceUnion=False, error=str(error))
        errors.append('source-mask-conservation')
    active = [r for r in rows if r['in_scope']]
    scoped = sum(r['area'] for r in active)
    if page['scope_ink_pixels'] is not None and scoped != page['scope_ink_pixels']:
        errors.append('scope-pixel-count')
    by_word, by_sub = defaultdict(list), defaultdict(list)
    for row in active:
        by_word[row['word_id']].append(row)
        by_sub[row['subword_id']].append(row)
    subs = [dict(r) for r in db.execute('SELECT * FROM subwords WHERE page_id=? AND in_scope=1', (pid,))]
    native_characters = {}
    by_native_word = defaultdict(list)
    for sub in proposal['subwords']:
        by_native_word[pid+':'+sub['wordId']].append(dict(sub, id=pid+':'+sub['id']))
    for wid, children in by_native_word.items():
        if wid in transcript:
            native_characters.update(subword_characters(transcript[wid], children))
    subchecks = [dict(check_subword(s, by_sub[s['id']], native_characters.get(s['id'])),
                      line=s['line'], wordId=s['word_id']) for s in subs]
    for check in subchecks:
        check['expectedSource'] = 'native-QuranTextByWord' if check['id'] in native_characters else 'native-alignment-unavailable'
        if check['id'] not in native_characters:
            check['issues'].append('native-subword-transcript-unaligned')
    proposal_sub = {pid+':'+s['id']: s for s in proposal['subwords']}
    path_index = {p['id']: p['runs'] for p in proposal['paths']}
    sub_paths = defaultdict(set)
    for c in proposal['candidates']:
        if c.get('subwordId'):
            sub_paths[pid+':'+c['subwordId']].update(c['pathIds'])
    by_parent = defaultdict(list)
    for s, check in zip(subs, subchecks):
        by_parent[s['word_id']].append(check)
        original = proposal_sub.get(s['id'])
        if not original or original['text'] != s['text'] or original['characters'] != json.loads(s['expected']):
            check['issues'].append('subword-transcript-changed')
        check['alignment'] = alignment(by_sub[s['id']], [path_index[p] for p in sub_paths[s['id']]])
        if check['alignment']['mismatches']:
            check['issues'].append('subword-geometry-mismatch')
    cache_path = native/'output/segmentation-cache'/f'{pid}.json'
    cache = json.loads(cache_path.read_text()) if cache_path.exists() else None
    original_words = {pid+':'+w['id']: w for w in proposal['words']}
    words = []
    for r in db.execute('SELECT * FROM words WHERE page_id=? AND in_scope=1 ORDER BY line,ordinal', (pid,)):
        word = dict(r)
        members = by_word[word['id']]
        children = by_parent[word['id']]
        original = original_words.get(word['id'])
        # Native initialized text is independent of proposal/cache text and ownership.
        native_word = transcript.get(word['id'])
        characters = native_word['characters'] if native_word else []
        expected = expected_inventory(characters) if native_word else None
        inventories = {s: inventory_check(expected, members, s) for s in SOURCES}
        issues = set(error for s in children for error in s['issues'])
        word_alignment = alignment(members, [path_index[p] for p in (original or {}).get('pathIds', [])])
        if word_alignment['mismatches']:
            issues.add('word-geometry-mismatch')
        if not inventories['final']['matches']:
            issues.add('word-mark-inventory')
        if not word['transcript_matches'] or not original or original['text'] != word['text']:
            issues.add('word-transcript-mismatch')
        if not native_word or native_word['text'] != word['text']:
            issues.add('native-word-transcript-mismatch')
        if word['flags'] != 1:
            issues.add('legacy-segmentation-warning')
        if not children and teacher().has_letter_body(word['text']):
            issues.add('no-subword-body')
        if any(i['subword_id'] is None and (i['final_kind'] in ('dot', 'mark') or
               (i['final_kind'] == 'symbol' and (i['final_type'] or '').startswith('waqf-')))
               for i in members) and children:
            issues.add('mark-without-subword')
        # Per-word exact union against original vector paths is renderer-dependent;
        # retain the overlap diagnostics, never rewrite source pixels to make a fit.
        check = dict(id=word['id'], pageId=pid, line=word['line'], ordinal=word['ordinal'],
            text=word['text'], bbox=[word[k] for k in ('x','y','w','h')],
            transcriptText=native_word['text'] if native_word else None,
            expectedSource='native-QuranTextByWord' if native_word else 'native-word-missing',
            inventories=inventories, subwords=children, issues=sorted(issues), alignment=word_alignment,
            letterCuts=letter_status(cache, word, children, config, version, prov),
            status='needs-review' if issues or errors else 'inventory-consistent',
            ownership='unverified', provenanceIssues=errors,
            items=[dict(id=i['id'], role=i['final_kind'], type=i['final_type'],
                        score=i['model_confidence'], subwordId=i['subword_id'],
                        reviewed=bool(i['human_reviewed']), bbox=[i[k] for k in ('x','y','w','h')]) for i in members])
        words.append(check)
    hints = ownership_hints(words, active) + ownership_hints(subchecks, active, same_word=True)
    unassigned = [dict(id=i['id'], role=i['final_kind'], type=i['final_type'],
                       bbox=[i[k] for k in ('x','y','w','h')], area=i['area']) for i in active if not i['word_id']]
    unassigned_hints = []
    for item in active:
        if item['word_id']:
            continue
        candidates = []
        for word in words:
            x, y, w, h = word['bbox']
            if (item['x'] >= x+w or item['x']+item['w'] <= x or
                    item['y'] >= y+h or item['y']+item['h'] <= y):
                continue
            paths = [path_index[p] for p in original_words[word['id']]['pathIds']]
            coverage = alignment([item], paths)['minimumSourceCoverage']
            if coverage >= .8:
                candidates.append(dict(wordId=word['id'], sourceCoverage=coverage))
                word.setdefault('unassignedInkCandidates', []).append(item['id'])
                if 'unassigned-ink-overlaps-word' not in word['issues']:
                    word['issues'].append('unassigned-ink-overlaps-word')
                word['status'] = 'needs-review'
        if candidates:
            unassigned_hints.append(dict(itemId=item['id'], role=item['final_kind'],
                type=item['final_type'], candidates=candidates,
                status='unassigned-ownership-proposal', uniqueVectorOwner=len(candidates) == 1))
    return dict(pageId=pid, width=page['width'], height=page['height'], sourceSha256=page['source_sha'],
        split=page['split'], sourceKind=page['source_kind'], proposalSha256=page['proposal_sha'],
        geometry=geometry, provenanceIssues=errors, scopePixels=scoped,
        words=words, ownershipHints=hints, unassigned=unassigned,
        unassignedOwnershipHints=unassigned_hints)


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2))


def audit(root, native, output=None, pages=None, progress=print):
    root, native = Path(root).resolve(), Path(native).resolve()
    output = Path(output).resolve() if output else root/'word-audit'
    if output == root or output == native:
        raise ValueError('Choose a separate audit output folder')
    output.mkdir(parents=True, exist_ok=True)
    (output/'pages').mkdir(exist_ok=True)
    transcript, transcript_provenance = export_transcript(native, output/'native-transcript.json')
    index_path=input_root(root)/'corpus.json'
    index = json.loads(index_path.read_text())
    config = json.loads((native/'templates/letter-forms.json').read_text())
    version = int(re.search(r'constexpr int CacheVersion = (\d+)', (native/'lettersegmentationcache.cpp').read_text())[1])
    summary = dict(schemaVersion=1, pages=0, words=0, subwords=0, statuses=Counter(),
        wordInventoryMatches=0, subwordInventoryMatches=0, issues=Counter(),
        missingTypes=Counter(), extraTypes=Counter(), unassignedRegions=0,
        unassignedPixels=0, ownershipHints=0, unassignedOwnershipHints=0,
        letterCuts=Counter(), provenanceIssues=Counter(),
        exactSourceUnionPages=0, independentlyVerifiedWords=0,
        transcriptSha256=digest_file(native/'quran.cpp'),
        transcriptProvenance=transcript_provenance,
        policy='Transcript-constrained consistency audit, not measured segmentation accuracy. Identical marks may exchange owners without changing counts. No annotations, masks, predictions or ownership are modified.',
        scope='Current corpus in_scope flags; excluded artwork, titles, verse markers and basmala openings are not silently reinstated.')
    page_summaries = []
    with closing(sqlite3.connect((root/'corpus.sqlite').as_uri()+'?mode=ro', uri=True)) as db, \
            (output/'words.jsonl').open('w') as records, (output/'words.csv').open('w', newline='') as csvfile:
        db.row_factory = sqlite3.Row
        db.execute('BEGIN')  # consistent read snapshot, never migrate/write corpus
        inference = db.execute("SELECT value FROM meta WHERE key='inference'").fetchone()
        summary['inference'] = json.loads(inference[0]) if inference else None
        writer = csv.DictWriter(csvfile, fieldnames=['id','text','status','issues','missing','extra','letterCuts'])
        writer.writeheader()
        for page in db.execute('SELECT * FROM pages ORDER BY page_id').fetchall():
            if pages and page['page_id'] not in pages:
                continue
            document = audit_page(db, root, native, dict(page), index, config, version, transcript[page['page_id']])
            pid = page['page_id']
            # Separate page scripts allow local-file browsing without fetch/CORS or a server.
            encoded = json.dumps(document, ensure_ascii=False, separators=(',', ':')).replace('<', '\\u003c')
            (output/'pages'/f'{pid}.js').write_text('window.showPage('+encoded+');')
            words = document['words']
            summary['pages'] += 1
            summary['exactSourceUnionPages'] += int(document['geometry']['exactSourceUnion'])
            summary['provenanceIssues'].update(document['provenanceIssues'])
            summary['unassignedRegions'] += len(document['unassigned'])
            summary['unassignedPixels'] += sum(i['area'] for i in document['unassigned'])
            summary['ownershipHints'] += len(document['ownershipHints'])
            summary['unassignedOwnershipHints'] += len(document['unassignedOwnershipHints'])
            for word in words:
                summary['words'] += 1
                summary['subwords'] += len(word['subwords'])
                summary['statuses'][word['status']] += 1
                summary['issues'].update(word['issues'])
                summary['wordInventoryMatches'] += int(word['inventories']['final']['matches'])
                summary['subwordInventoryMatches'] += sum(s['inventories']['final']['matches'] for s in word['subwords'])
                summary['missingTypes'].update(word['inventories']['final']['missing'])
                summary['extraTypes'].update(word['inventories']['final']['extra'])
                summary['letterCuts'][word['letterCuts']['status']] += 1
                records.write(json.dumps(word, ensure_ascii=False, separators=(',', ':'))+'\n')
                writer.writerow(dict(id=word['id'], text=word['text'], status=word['status'],
                    issues='; '.join(word['issues']+word['provenanceIssues']),
                    missing=json.dumps(word['inventories']['final']['missing'], ensure_ascii=False),
                    extra=json.dumps(word['inventories']['final']['extra'], ensure_ascii=False), letterCuts=word['letterCuts']['status']))
            page_summaries.append(dict(pageId=pid, words=len(words),
                flagged=sum(w['status']=='needs-review' for w in words),
                mismatches=sum(not w['inventories']['final']['matches'] for w in words),
                unassigned=len(document['unassigned']), provenanceIssues=document['provenanceIssues']))
            if summary['pages'] % 25 == 0:
                progress(f"Audited {summary['pages']} pages / {summary['words']} words")
    summary['pageSummary'] = page_summaries
    write_json(output/'summary.json', summary)
    template = (Path(__file__).parent/'word_audit.html').read_text()
    # Paths support a custom output folder as well as the default.
    import os
    render = Path(os.path.relpath(root/'render', output)).as_posix()
    (output/'index.html').write_text(template.replace('__SUMMARY__', json.dumps(summary, ensure_ascii=False).replace('<','\\u003c')).replace('__RENDER__', escape(render, quote=True)))
    return summary
