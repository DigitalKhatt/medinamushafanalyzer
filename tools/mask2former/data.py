"""Freeze connected subword masks from the current native cutter."""
from collections import Counter, defaultdict
from contextlib import closing
import io
from pathlib import Path
import random
import shutil
import sqlite3

from workspace import DATA, NATIVE, read

import numpy as np

from letter_bases import base_character_count, check_training_records
from letter_corpus_audit import document_for, prepare_example, source_records, write_json
from letter_retest import SEED
from letter_retest_data import weak_partition
from letter_template_repeat import TOOTH_TEMPLATES, cases, choose_diverse, complete_row, sha, tooth_policy, vector_row
from seen import families, layout, measure


def open_audit(path):
    return sqlite3.connect(path.resolve().as_uri() + '?mode=ro', uri=True)


def base_count(row):
    return base_character_count(row['text'])


def connected_rows(cache):
    return [row for row in source_records(cache) if base_count(row) >= 2]


def identifier(page, row):
    return f"{page}:l{row['line']}-w{row['word']}-s{row['subword']}"


def generation_for(snapshot):
    generation = read(snapshot / 'generation.json')
    if generation.get('completedPages') != list(range(1, 605)) or generation.get('failures'):
        raise ValueError(f'Incomplete native snapshot at {snapshot}')
    if generation.get('trackerEnabled') or generation.get('humanReviewed'):
        raise ValueError('Use the automatic cutter snapshot with the tracker disabled')
    if sha(snapshot / 'letter-forms.json') != generation['templateFileSha256']:
        raise ValueError('Native template snapshot changed')
    return generation


def load_page(snapshot, generation, page):
    path = snapshot / 'pages' / f'page{page:03}.json'
    if sha(path) != generation['pageHashes'][str(page)]['export']:
        raise ValueError(f'Native export changed on page {page}')
    return read(path)


def document(page, cache, info):
    raw = read(NATIVE / 'output' / 'vector-review' / f'{page}.raw.json')
    data = document_for(cache, raw)
    if data['sourceSha256'] != info['sha256']:
        raise ValueError(f'Artwork and page inventory disagree for {page}')
    return data


def matching_audit(previous, generation_hash):
    if previous is None:
        return None
    folder = previous / 'corpus-audit'
    if not (folder / 'summary.json').is_file() or not (folder / 'comparison.sqlite').is_file():
        return None
    summary = read(folder / 'summary.json')
    update = summary.get('referenceUpdate', {})
    signature = read(folder / 'signature.json') if (folder / 'signature.json').is_file() else {}
    snapshot = update.get('generationSha256', signature.get('snapshot'))
    if snapshot == generation_hash and summary['pages'] == 604 and not summary['failures']:
        return folder / 'comparison.sqlite'
    return None


def mine_training(audit, pool, snapshot, generation, pages, focus, limit=60):
    """Mine train-page errors only when their reference masks still match."""
    if audit is None:
        return {}
    candidates = []
    with closing(open_audit(audit)) as db:
        for item in pool:
            if item['id'] in focus or not (item['families'] or item['triple']):
                continue
            saved = db.execute("""SELECT masks FROM examples
                               WHERE id=? AND split='train' AND reference='complete'
                               AND json_extract(detail,'$.expectedBodies') >= 2 AND count_error > 0""",
                               (item['id'],)).fetchone()
            if saved is None:
                continue
            with np.load(io.BytesIO(saved[0])) as data:
                stats = measure(item['letters'], data['reference'], data['prediction'], data['predicted_roles'])
            if stats['fragments'] >= 2:
                candidates.append((item, 'seen-fragmented', stats['fragments']))
            elif item['triple'] and any(run['merged'] for run in stats['triples']):
                candidates.append((item, 'separate-teeth-hard', 1))

        random.Random(SEED).shuffle(candidates)
        candidates.sort(key=lambda entry: -entry[2])
        counts, signatures = Counter(), defaultdict(set)
        for item, reason, _ in candidates:
            if counts[reason] >= limit or item['signature'] in signatures[reason]:
                continue
            page, sid = item['page'], item['id']
            cache = load_page(snapshot, generation, int(page[4:]))
            row = next(r for r in connected_rows(cache) if identifier(page, r) == sid)
            current = prepare_example(row, document(page, cache, pages[page]))
            if not current['status']['valid']:
                continue
            blob, = db.execute('SELECT masks FROM examples WHERE id=?', (sid,)).fetchone()
            with np.load(io.BytesIO(blob)) as saved:
                if any(not np.array_equal(current[name], saved[name]) for name in ('ink', 'reference', 'roles')):
                    continue
            focus[sid] = reason
            signatures[reason].add(item['signature'])
            counts[reason] += 1
    return dict(counts)


def add_diverse_focus(pool, focus, per_family=30, tooth_controls=80):
    order = list(pool)
    random.Random(SEED).shuffle(order)
    counts, signatures = Counter(), defaultdict(set)
    for item in order:
        if item['id'] in focus:
            continue
        for family in item['families']:
            if counts[family] < per_family and item['signature'] not in signatures[family]:
                focus[item['id']] = 'seen-' + family
                counts[family] += 1
                signatures[family].add(item['signature'])
                break
    for item in order:
        if counts['three-teeth'] >= tooth_controls:
            break
        if item['triple'] and item['id'] not in focus and item['signature'] not in signatures['three-teeth']:
            focus[item['id']] = 'separate-teeth-control'
            signatures['three-teeth'].add(item['signature'])
            counts['three-teeth'] += 1


def load_corpus(previous):
    # Previous splits are kept when continuing a comparison.
    frozen = previous / 'inputs' / 'corpus.json' if previous else None
    return read(frozen if frozen and frozen.is_file() else DATA / 'corpus.json')


def prepare(folder, snapshot, pretrained, previous, focus_ids, excluded_ids, include_ids=(), add_test_focus=()):
    if folder.exists():
        raise ValueError(f'Choose a fresh run directory. Already exists: {folder}')
    if not all((pretrained / name).is_file() for name in ('config.json', 'model.safetensors')):
        raise ValueError('Choose a local Mask2Former Swin-Tiny pretrained directory')
    generation = generation_for(snapshot)
    generation_hash = sha(snapshot / 'generation.json')
    corpus = load_corpus(previous)
    pages = {p['id']: p for p in corpus['pages']}
    if set(pages) != {f'page{page:03}' for page in range(1, 605)} or len(corpus['pages']) != 604:
        raise ValueError('The page inventory must contain each of the 604 pages exactly once')
    if {page['split'] for page in pages.values()} != {'train', 'validation', 'test'}:
        raise ValueError('The page inventory must define train, validation and test splits')
    add_test_focus = set(add_test_focus)
    promoted = set()
    if add_test_focus:
        if not previous or focus_ids or excluded_ids or include_ids:
            raise ValueError('Adding test crops requires a previous run and a single focus list')
        if read(previous / 'run.json')['generationSha256'] != generation_hash:
            raise ValueError('Use the previous frozen cutter snapshot when adding test crops')
        promoted = {sid.split(':', 1)[0] for sid in add_test_focus}
        if not all(pages.get(page, {}).get('split') == 'test' for page in promoted):
            raise ValueError('All added focus IDs must come from previous test pages')
        for page in promoted:
            pages[page]['split'] = 'train'
        focus_ids = add_test_focus
    prior = read(previous / 'dataset.json') if previous else {}
    reviewed = set(prior.get('excludedReviewedIds', []))
    excluded_ids |= reviewed | set(prior.get('excludedCropIds', []))
    if focus_ids & excluded_ids:
        raise ValueError(f'Focus IDs also excluded: {sorted(focus_ids & excluded_ids)}')

    pools = defaultdict(list)
    scope = Counter()
    for page, info in sorted(pages.items()):
        cache = load_page(snapshot, generation, int(page[4:]))
        rows = source_records(cache)
        words = {(row['line'], row['word']) for row in rows}
        lines = {row['line'] for row in cache['sourceLines'] if row['transcriptWords']}
        if any(row.get('inTranscript', True) and row['line'] in lines and base_count(row)
               and (row['line'], row['word']) not in words for row in cache['sourceWords']):
            raise ValueError(f'A transcribed word has no native subword ink on {page}')
        connected = [row for row in rows if base_count(row) >= 2]
        scope['connected'] += len(connected)
        scope['singleLetterPassthrough'] += sum(base_count(row) == 1 for row in rows)
        split = info['split']
        for row in connected:
            sid = identifier(page, row)
            if sid in excluded_ids or not complete_row(row, set(), sid):
                continue
            letters = row['letters']
            seen, triples = layout(letters)
            _, _, width, height = row['bounds']
            signature = (row['baseText'], round(2 * width / max(.01, height)))
            pools[split].append(dict(id=sid, page=page, cases=cases(row), signature=signature,
                                     families=families(letters, seen), triple=bool(triples),
                                     letters=letters if split == 'train' and (seen or triples) else ()))
    eligible = {item['id'] for item in pools['train']}
    if focus_ids - eligible:
        raise ValueError(f'Focus IDs need a complete multi-letter cut on a training page: {sorted(focus_ids - eligible)}')
    include_ids = set(include_ids)
    if include_ids - eligible:
        raise ValueError(f'Included IDs need a complete multi-letter cut on a training page: {sorted(include_ids - eligible)}')
    focus = {r['id']: r['trainingFocus'] for r in prior.get('records', [])
             if r['id'] in eligible and r.get('trainingFocus')}
    cache_audit = matching_audit(previous, generation_hash)
    mined = {}
    if not promoted:
        mining_audit = cache_audit
        if mining_audit is None and previous:
            audit_folder = previous / 'corpus-audit'
            if (audit_folder / 'summary.json').is_file() and (audit_folder / 'comparison.sqlite').is_file():
                summary = read(audit_folder / 'summary.json')
                if summary.get('pages') == 604 and not summary.get('failures'):
                    mining_audit = audit_folder / 'comparison.sqlite'
        mined = mine_training(mining_audit, pools['train'], snapshot, generation, pages, focus)
    for item in pools['train']:
        if item['id'] in focus_ids:
            if not item['families']:
                raise ValueError(f"Manual seen focus needs a sīn or shīn: {item['id']}")
            focus[item['id']] = 'seen-selected'
    if not promoted:
        add_diverse_focus(pools['train'], focus)

    selected = {}
    for split, rounds, limit in [('train', 16, 4000), ('validation', 3, 700), ('test', 2, 500)]:
        pool = pools[split]
        # Reuse the previous held-out crop IDs when possible for direct comparison.
        old_ids = {r['id'] for r in prior.get('records', []) if r['split'] == split}
        picks = [item for item in pool if item['id'] in old_ids]
        if not promoted and (not picks or split == 'train'):
            candidates = choose_diverse(pool, rounds, min(limit, len(pool)), SEED)
            if split == 'train' and picks:
                covered = Counter(key for item in picks for key in item['cases'])
                available = Counter(key for item in pool for key in item['cases'])
                chosen = {item['id'] for item in picks}
                for item in candidates:
                    if item['id'] not in chosen and any(covered[key] < min(8, available[key]) for key in item['cases']):
                        picks.append(item)
                        covered.update(item['cases'])
            else:
                picks = candidates
        selected.update({item['id']: item for item in picks})
    selected.update({item['id']: item for item in pools['train'] if item['id'] in focus or item['id'] in include_ids})

    folder.mkdir(parents=True)
    for name in ('masks', 'inputs'):
        (folder / name).mkdir()
    shutil.copytree(pretrained, folder / 'pretrained')
    write_json(folder / 'inputs' / 'corpus.json', corpus)
    shutil.copyfile(snapshot / 'generation.json', folder / 'inputs' / 'native-generation.json')
    shutil.copyfile(snapshot / 'letter-forms.json', folder / 'inputs' / 'letter-forms.json')
    by_page = defaultdict(set)
    for item in selected.values():
        by_page[item['page']].add(item['id'])
    records, rejected, tooth_areas = [], [], []
    for page, ids in sorted(by_page.items()):
        cache = load_page(snapshot, generation, int(page[4:]))
        split = pages[page]['split']
        source = document(page, cache, pages[page])
        transform = source['pdfToRaster']
        scale = abs(transform[0] * transform[3] - transform[1] * transform[2])
        for row in connected_rows(cache):
            sid = identifier(page, row)
            if sid not in ids:
                continue
            vector = vector_row(row, source)
            try:
                if [body['letter'] for body in row['bodies']] != list(row['baseText']):
                    raise ValueError('body-order')
                labels, roles, bbox, repaired, clipped = weak_partition(vector, source['raster']['width'])
                if int((roles == 1).sum()) != base_count(row):
                    raise ValueError('body-count')
            except ValueError as error:
                rejected.append(dict(id=sid, split=split, reason=str(error)))
                continue
            file = Path('masks') / (sid.replace(':', '__') + '.npz')
            np.savez_compressed(folder / file, instances=labels, roles=roles)
            record = dict(id=sid, page=page, split=split, group='weak_' + split, provenance='machine-weak',
                          textForDisplayOnly=row['text'], bodyRegions=int((roles == 1).sum()),
                          bbox=bbox, file=str(file), maskSha256=sha(folder / file), pdfToRaster=transform,
                          sourceSha256=source['sourceSha256'], sourceCacheSha256=source['sourceCacheSha256'],
                          templateSha256=source['templateSha256'], templates=row['templates'], letters=row['letters'],
                          repairedPixels=repaired, clippedExtraPixels=clipped)
            if sid in focus:
                record['trainingFocus'] = focus[sid]
            records.append(record)
            if split == 'train':
                for letter in row['letters']:
                    if letter.get('template') in TOOTH_TEMPLATES:
                        tooth_areas.append(dict(analyzerArea=float((labels == letter['letterIndex']).sum() / scale)))
    records.sort(key=lambda row: row['id'])
    check_training_records(records)
    missing = (focus_ids | include_ids) - {row['id'] for row in records}
    if missing:
        raise ValueError(f'Explicit focus crops failed the ink checks: {sorted(missing)}')
    if promoted:
        inherited = {r['id']: (r['split'], r['maskSha256']) for r in prior['records'] if r['page'] not in promoted}
        retained = {r['id']: (r['split'], r['maskSha256']) for r in records if r['id'] not in add_test_focus}
        if inherited != retained:
            raise ValueError('The existing sample or masks changed while adding test crops')
    decoder = prior.get('decoder')
    if not decoder:
        decoder, _ = tooth_policy(tooth_areas, records)
    groups = Counter(row['group'] for row in records)
    if set(groups) != {'weak_train', 'weak_validation', 'weak_test'}:
        raise ValueError('All splits need valid connected training or evaluation crops')
    manifest = dict(schemaVersion=2, seed=SEED, records=records, groups=dict(groups),
                    target='Letter body and kashida instances', tuningGroup='weak_validation',
                    excludedReviewedIds=sorted(reviewed), excludedCropIds=sorted(excluded_ids - reviewed), decoder=decoder,
                    minimumTrainingBaseCharacters=2, sampling=dict(coverEveryTrainingExample=True))
    if promoted:
        manifest.update(addedTestFocusIds=sorted(add_test_focus), promotedTestPages=sorted(promoted))
    write_json(folder / 'dataset.json', manifest)
    write_json(folder / 'run.json', dict(snapshot=str(snapshot), generationSha256=generation_hash,
               corpusSha256=sha(folder / 'inputs' / 'corpus.json'), previous=str(previous) if previous else None,
               scope=dict(scope), previousReferenceMatched=bool(cache_audit), datasetSha256=sha(folder / 'dataset.json'),
               pretrainedFiles={p.name: sha(p) for p in (folder / 'pretrained').iterdir() if p.is_file()}))
    write_json(folder / 'preparation.json', dict(groups=dict(groups), scope=dict(scope), rejected=rejected,
               includedCropIds=sorted(include_ids), mined=mined,
               focus=dict(Counter(r['trainingFocus'] for r in records if 'trainingFocus' in r))))
    print(f'Frozen targets {dict(groups)}. Single-letter subwords kept as is: {scope["singleLetterPassthrough"]}.', flush=True)
