"""Freeze template-diverse complete cuts and report image-only unfinished predictions.

Generated data stays under data/automatic. Native caches, artwork and reviews
are read-only. Qt rasterization mirrors proposalexport.cpp (alpha >= 99).
"""
from workbench_paths import NATIVE
import argparse
from collections import Counter, defaultdict
import hashlib
from html import escape
import json
import math
import os
from pathlib import Path
import random
import shutil
import sys
import time

import numpy as np
from PIL import Image
from letter_bases import MIN_TRAINING_BASES, base_character_count, has_training_boundary

ROOT = Path(__file__).resolve().parent
TOOTH_TEMPLATES = {'beh-init', 'beh-shallow-step-init', 'beh-upright-medi', 'beh-tooth-medi'}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2)+'\n')


def audit_module():
    sys.path.insert(0, str(NATIVE/'tools'))
    import report_incomplete_corpus
    return report_incomplete_corpus


def qt_path(data):
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QPainterPath
    path = QPainterPath()
    for command in data.get('commands', []):
        name, *values = command
        if name == 'M': path.moveTo(*values)
        elif name == 'L': path.lineTo(*values)
        elif name == 'C': path.cubicTo(*values)
        else: raise ValueError(f'Unsupported path command: {name}')
    path.setFillRule(Qt.FillRule.WindingFill if data.get('fillRule') == 'nonzero' else Qt.FillRule.OddEvenFill)
    return path


def raster_runs(data, width, height, transform):
    """Keep page coordinates, all contours and counters, and the recorded scale."""
    from PySide6.QtCore import Qt, QRect
    from PySide6.QtGui import QTransform, QImage, QPainter
    sx, b, c, sy, tx, ty = transform
    if any(abs(v) > 1e-10 for v in (b, c, tx, ty)):
        raise ValueError('Unsupported source transform')
    path = QTransform.fromScale(sx, sy).map(qt_path(data))
    box = path.boundingRect().toAlignedRect().adjusted(-2, -2, 2, 2).intersected(QRect(0, 0, width, height))
    if box.isEmpty(): return []
    image = QImage(box.size(), QImage.Format.Format_RGBA8888)
    image.fill(Qt.GlobalColor.transparent)
    painter = QPainter(image)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.translate(-box.x(), -box.y())
    painter.fillPath(path, Qt.GlobalColor.black)
    painter.end()
    buffer = np.frombuffer(image.constBits(), np.uint8).reshape(box.height(), image.bytesPerLine())
    ink = buffer[:, :box.width()*4].reshape(box.height(), box.width(), 4)[:, :, 3] >= 99
    runs = []
    for y, row in enumerate(ink):
        changes = np.diff(np.pad(row.astype('int8'), 1))
        starts, ends = np.flatnonzero(changes == 1), np.flatnonzero(changes == -1)
        for start, end in zip(starts, ends):
            runs.extend([(y+box.y())*width+int(start)+box.x(), int(end-start)])
    return runs


def vector_row(row, document):
    raster, transform = document['raster'], document['pdfToRaster']
    width, height = raster['width'], raster['height']
    regions = []
    for body in row['bodies']:
        regions.append(dict(role='body', letter=body['letter'], labelSource=body.get('labelSource'),
                            runs=raster_runs(body, width, height, transform)))
    for join in row['joins']:
        path = join.get('kashida')
        if path and path.get('commands'):
            runs = raster_runs(path, width, height, transform)
            if runs: regions.append(dict(role='kashida', runs=runs))
    return dict(id=document['page']+f":l{row['line']}-w{row['word']}-s{row['subword']}",
                text=row['text'], complete=row['complete'], provisional=row.get('provisional', False),
                templates=row.get('templates', []), letters=row.get('letters', []), regions=regions,
                sourceRuns=raster_runs(row['sourceInk'], width, height, transform))


def cases(row):
    selected = set()
    for letter in row.get('letters', []):
        for key, template in letter.items():
            if template and isinstance(template, str) and (key == 'template' or key.endswith('Template')):
                selected.add(f"{template}|{letter['position']}|{letter['character']}")
    # Boundary refinements can be recorded only on the assembled subword.
    selected.update(f"{template}|used|*" for template in row.get('templates', []))
    return sorted(selected)


def choose_diverse(pool, per_case, limit, seed):
    """Round-robin rare cases first, preferring different text and aspect ratios."""
    rng = random.Random(seed)
    order = list(pool)
    rng.shuffle(order)
    groups = defaultdict(list)
    for item in order:
        for case in item['cases']: groups[case].append(item)
    selected = {}; seen = defaultdict(set); coverage = Counter()
    for round_index in range(per_case):
        for key in sorted(groups, key=lambda k: (len(groups[k]), k)):
            if coverage[key] > round_index: continue
            available = [i for i in groups[key] if i['id'] not in selected]
            if not available: continue
            choice = next((i for i in available if i['signature'] not in seen[key]), available[0])
            selected[choice['id']] = choice
            for case in choice['cases']:
                coverage[case] += 1; seen[case].add(choice['signature'])
    if len(selected) > limit:
        raise ValueError(f'Coverage needs {len(selected)} examples; increase selection limit {limit}')
    # Broaden joining contexts and subword lengths beyond the rule quotas.
    signatures = {i['signature'] for i in selected.values()}
    for item in order:
        if len(selected) >= limit: break
        if item['id'] not in selected and item['signature'] not in signatures:
            selected[item['id']] = item; signatures.add(item['signature'])
    for item in order:
        if len(selected) >= limit: break
        selected.setdefault(item['id'], item)
    return list(selected.values())


def complete_row(row, incomplete_ids, identifier, include_provisional=False):
    if include_provisional and row.get('provisional'):
        return (bool(row.get('sourceInk')) and len(row['bodies']) == len(row['letters'])
                and all(letter['status'] in {'matched', 'not-required', 'remainder'} for letter in row['letters']))
    return (row['complete'] and not row.get('provisional') and identifier not in incomplete_ids
            and row.get('sourceInk') and all(l['status'] in {'matched', 'not-required'} for l in row['letters'])
            and not any(b.get('labelSource') == 'remainder' or b.get('reviewRequired') for b in row['bodies']))


def cover_used_templates(pools, pages, used, protected_validation):
    """Move entire eligible validation pages to training for otherwise absent forms."""
    def represented(items):
        return {case.split('|')[0] for item in items for case in item['cases']
                if case.endswith('|used|*')}

    overrides = {}
    missing = set(used) - represented(pools['train'])
    while missing:
        candidates = defaultdict(list)
        for item in pools['validation']:
            if item['page'] not in protected_validation and item['baseCharacters'] >= MIN_TRAINING_BASES:
                candidates[item['page']].append(item)
        possible = [(len(represented(items) & missing), number)
                    for number, items in candidates.items()]
        count, number = max(possible, key=lambda item: (item[0], -item[1]), default=(0, 0))
        if count == 0:
            break
        moved = [item for item in pools['validation'] if item['page'] == number]
        pools['validation'] = [item for item in pools['validation'] if item['page'] != number]
        pools['train'].extend(item for item in moved if item['baseCharacters'] >= MIN_TRAINING_BASES)
        covered = represented(candidates[number]) & missing
        pages[number]['split'] = 'train'
        overrides[f'page{number:03}'] = dict(previous='validation', split='train',
                                           reason='Cover otherwise absent used templates', templates=sorted(covered))
        missing -= represented(pools['train'])
    return overrides


def tooth_policy(tooth_areas, records):
    """Estimate the physical minimum region area from the retained training ink."""
    if not tooth_areas: raise ValueError('No training-only tooth references')
    mean_tooth = float(np.mean([r['analyzerArea'] for r in tooth_areas]))
    # Current raster transforms have tiny page-size rounding differences. Each
    # record carries its exact determinant so decoder thresholds remain physical.
    representative_scale = float(np.mean([r['pdfToRaster'][0]*r['pdfToRaster'][3] for r in records if r['group']=='weak_train']))
    cutoff = mean_tooth*.1*representative_scale
    decoder = dict(policy='one-tenth-mean-training-tooth-area', fraction=.1,
                   toothTemplates=sorted(TOOTH_TEMPLATES), toothTrainingInstances=len(tooth_areas),
                   meanToothAnalyzerArea=mean_tooth, referenceRasterAreaScale=representative_scale,
                   meanToothRasterArea=mean_tooth*representative_scale, minimumRegionAreaPixels=cutoff,
                   minimumRegionAreaPixelsRoundedUp=math.ceil(cutoff),
                   appliesTo='supported source-ink area of each predicted body or connection instance, before completion',
                   sourceInkPolicy='Filtered regions do not delete ink: completion assigns it to accepted regions; no accepted region leaves explicit unknown ink.',
                   estimatorSplit='train only')
    tooth_statistics=dict(minimum=min(r['analyzerArea'] for r in tooth_areas),
        median=float(np.median([r['analyzerArea'] for r in tooth_areas])),
        maximum=max(r['analyzerArea'] for r in tooth_areas))
    return decoder, tooth_statistics


def prepare(args):
    from letter_retest_data import weak_partition, human_record
    from letter_cut_pilot import EXAMPLES
    audit = audit_module()
    folder, snapshot = args.folder.resolve(), args.snapshot.resolve()
    if folder.exists(): raise ValueError('Use a new experiment directory')
    generation = json.loads((snapshot/'generation.json').read_text())
    config = json.loads((snapshot/'letter-forms.json').read_text())
    if generation['pagesCompleted'] != 604 or generation['failures']: raise ValueError('Wait for all 604 pages')
    if sha(snapshot/'letter-forms.json') != generation['templateFileSha256']: raise ValueError('Changed frozen templates')
    # The exporter is frozen for the whole run. Concurrent edits to the live
    # cutter do not change already running native processes or this snapshot.
    frozen_exporter=snapshot/'exporter'
    if frozen_exporter.exists() and sha(frozen_exporter)!=generation['exporterSha256']:
        raise ValueError('Changed frozen native exporter')
    corpus = json.loads((args.data/'corpus.json').read_text())
    pages = {int(p['id'][4:]): p for p in corpus['pages']}
    old = args.review_index or args.data/'automatic/experiments/letter-cut-benchmark'
    reviewed_validation = json.loads((old/'validation-proposals.json').read_text())['examples']
    import csv
    queue = list(csv.DictReader((old/'review-queue.csv').open()))
    reviewed_training = [
        (page, part, next(r['subword_id'] for r in queue if r['subword_id'].startswith(page+':')))
        for page, part in EXAMPLES.items()
    ]
    reviewed_ids = {identifier for _, _, identifier in reviewed_training}
    reviewed_ids.update(row['subwordId'] for row in reviewed_validation)
    human = []
    if not args.cutter_only:
        for page, part, identifier in reviewed_training:
            human.append(human_record(args.data, page, part, 1, identifier, 'train'))
        for row in reviewed_validation:
            human.append(human_record(args.data, row['page'], row['regionId'], row['annotationRevision'], row['subwordId'], 'validation'))
    for record, _, _ in human:
        if pages[int(record['page'][4:])]['split'] != record['split']:
            raise ValueError(f"Reviewed subword on the wrong split: {record['id']}")
    partition_failures = set()
    partition_file = snapshot/'ink-partition.json'
    if partition_file.is_file():
        partition = json.loads(partition_file.read_bytes())
        if partition['generationSha256'] != sha(snapshot/'generation.json'):
            raise ValueError('Ink ownership audit belongs to a different snapshot')
        partition_failures = {row['id'] for row in partition['failures']}
    pools = defaultdict(list); unfinished = []; page_hashes = {}; available = defaultdict(Counter)
    used_templates = defaultdict(Counter)
    page_template_usage = defaultdict(Counter)
    excluded = Counter(); audit_totals = Counter()
    for number in range(1, 605):
        path = snapshot/'pages'/f'page{number:03}.json'
        cache = json.loads(path.read_text())
        if cache['templateConfig'] != config or cache['sourceFileSha256'] != sha(NATIVE/f'pages/page{number}.dat'):
            raise ValueError(f'Changed cache or source page: {number}')
        page_hashes[str(number)] = sha(path)
        receipt = generation.get('pageHashes', {}).get(str(number))
        if receipt and receipt['export'] != page_hashes[str(number)]:
            raise ValueError(f'Export differs from frozen page receipt: {number}')
        analysis = audit.analyse_page(cache, partition_failures); audit_totals.update(analysis['summary'])
        unfinished.extend(analysis['items'])
        bad = {r['id'] for r in analysis['items']}
        split = pages[number]['split']
        lines = {l['line']: l for l in cache['sourceLines']}
        words = {(w['line'], w['word']) for w in cache['sourceWords'] if w.get('inTranscript', True)}
        for row in cache['subwords']:
            identifier = audit.identifier(number, row)
            if ((row['line'], row['word']) not in words or
                    lines.get(row['line'], {}).get('transcriptWords') == 0 or
                    not audit.has_letter(audit.body_letters(row['text']))):
                excluded['outside-text-scope'] += 1; continue
            used_templates[split].update(set(row.get('templates', [])))
            page_template_usage[number].update(set(row.get('templates', [])))
            if identifier in reviewed_ids:
                excluded['excluded-reviewed-crop' if args.cutter_only else 'human-override'] += 1
                continue
            if not complete_row(row, bad, identifier, args.include_provisional): excluded['not-complete'] += 1; continue
            if split == 'train' and base_character_count(row['text']) < MIN_TRAINING_BASES:
                excluded['fewer-than-two-training-bases'] += 1
                continue
            box = row['bounds']; ratio = round(box[2]/max(.01, box[3])*2)/2
            case_ids = cases(row)
            for case in case_ids: available[split][case] += 1
            if not case_ids and len(row['letters']) == 1: case_ids = ['isolated|'+row['letters'][0]['character']]
            pools[split].append(dict(id=identifier, page=number, cases=case_ids,
                                    baseCharacters=base_character_count(row['text']),
                                    signature=audit.body_letters(row['text'])+'|'+str(ratio)))
        if number % 100 == 0: print(f'Inventoried {number}/604 pages', flush=True)
    split_overrides = {}
    if args.cover_used_templates:
        all_used = set().union(*(set(counter) for counter in used_templates.values()))
        protected = {int(row['page'][4:]) for row in reviewed_validation}
        split_overrides = cover_used_templates(pools, pages, all_used, protected)
        available = defaultdict(Counter)
        used_templates = defaultdict(Counter)
        for split, pool in pools.items():
            for item in pool:
                available[split].update(case for case in item['cases'] if not case.startswith('isolated|'))
        for number, usage in page_template_usage.items():
            used_templates[pages[number]['split']].update(usage)
        print(json.dumps(dict(pageSplitOverrides=split_overrides)), flush=True)
    folder.mkdir(parents=True)
    for name in ['masks', 'vector', 'inputs', 'code', 'logs']: (folder/name).mkdir()
    shutil.copyfile(snapshot/'letter-forms.json', folder/'inputs/letter-forms.json')
    shutil.copyfile(snapshot/'generation.json', folder/'inputs/native-generation.json')
    write(folder/'inputs/corpus.json', corpus)
    if not args.cutter_only:
        for name in ('review-queue.csv', 'validation-proposals.json'):
            shutil.copyfile(old/name, folder/'inputs'/name)
    # A review already shown to the user can be narrowed without drawing
    # replacement crops. New experiments sample the eligible training pool.
    keep = args.retain_training_from
    prior_targets = None
    if keep:
        prior = json.loads((keep/'dataset.json').read_bytes())
        source = json.loads((keep/'inputs/snapshot.json').read_bytes())
        if (prior.get('trainingReview', {}).get('status') != 'open' or
                source['generationSha256'] != sha(snapshot/'generation.json') or
                source['corpusSha256'] != sha(args.data/'corpus.json') or
                json.loads((keep/'run.json').read_bytes())['datasetSha256'] != sha(keep/'dataset.json')):
            raise ValueError('Retain examples from an open review of the same native snapshot and page splits')
        prior_targets = {r['id']: r['maskSha256'] for r in prior['records']
                         if r['group'] == 'weak_train' and has_training_boundary(r)}
        # Retry the original raster rejections as well, to preserve coverage diagnostics.
        rejected = {r['id'] for r in json.loads((keep/'template-coverage.json').read_bytes())['rasterRejected']}
        retained_draw = prior_targets.keys() | rejected
    selected = {}
    for split, per_case, limit in [('train', 16, args.training_examples), ('validation', 3, 700), ('test', 2, 500)]:
        if split == 'train' and keep:
            selected[split] = [i for i in pools[split] if i['id'] in retained_draw]
        else:
            selected[split] = choose_diverse(pools[split], per_case, limit, 63)
    chosen = {i['id']: split for split, rows in selected.items() for i in rows}
    by_page = defaultdict(set)
    for identifier in chosen: by_page[int(identifier[4:7])].add(identifier)
    records = []; tooth_areas = []; raster_failures = []; sampled_usage = defaultdict(Counter); scale_areas = set()
    frozen_vectors = {}
    # On a geometry rejection, select a replacement from the same uncovered
    # cases below. Rejections never silently become training background.
    def save_record(record, labels, roles):
        filename = 'masks/'+record['id'].replace(':', '__')+'.npz'
        np.savez_compressed(folder/filename, instances=labels, roles=roles)
        record.update(file=filename, maskSha256=sha(folder/filename), regions=len(roles)-1,
                      bodyRegions=int((roles == 1).sum()), kashidaRegions=int((roles == 2).sum()),
                      inkPixels=int((labels > 0).sum()))
        records.append(record)
    def export_one(row, document, split):
        vector = vector_row(row, document)
        try: labels, roles, bbox, repaired, extra = weak_partition(vector, document['raster']['width'])
        except ValueError as error:
            raster_failures.append(dict(id=vector['id'], reason=str(error), cases=cases(row))); return False
        determinant = abs(document['pdfToRaster'][0]*document['pdfToRaster'][3])
        scale_areas.add(round(determinant, 8))
        record = dict(id=vector['id'], page=document['page'], split=split, group='weak_'+split,
                      provenance='machine-weak', textForDisplayOnly=row['text'], bbox=bbox,
                      sourceSha256=document['sourceSha256'], sourceCacheSha256=document['sourceCacheSha256'],
                      raster=document['raster'], pdfToRaster=document['pdfToRaster'], templateSha256=document['templateSha256'],
                      templates=row['templates'], templateCases=cases(row), letters=row['letters'],
                      provisional=bool(row.get('provisional')),
                      repairedPixels=repaired, clippedExtraPixels=extra)
        save_record(record, labels, roles)
        for case in cases(row): sampled_usage[split][case] += 1
        if split == 'train':
            for letter in row['letters']:
                if letter.get('template') in TOOTH_TEMPLATES:
                    index = letter['letterIndex']
                    tooth_areas.append(dict(id=record['id'], letterIndex=index, template=letter['template'],
                        rasterPixels=int((labels == index).sum()), analyzerArea=float((labels == index).sum()/determinant)))
        return vector
    # Keep a small native-vector document with only the selected examples.
    for number in sorted(by_page):
        cache = json.loads((snapshot/'pages'/f'page{number:03}.json').read_text())
        raw_path = NATIVE/'output/vector-review'/f'page{number:03}.raw.json'
        raw = json.loads(raw_path.read_text()); info = pages[number]
        if raw['sourceSha256'] != info['sha256'] or raw['provenance']['cacheSha256'] != cache['sourceFileSha256']:
            raise ValueError(f'Source raster provenance differs: {number}')
        document = dict(schemaVersion=1, page=info['id'], provenance='machine-weak', sourceSha256=info['sha256'],
                        sourceCacheSha256=cache['sourceFileSha256'], raster=raw['raster'], pdfToRaster=raw['provenance']['pdfToRaster'],
                        templateSha256=cache['letterFormsSha256'], cacheVersion=cache['version'], examples=[])
        for row in cache['subwords']:
            identifier = audit.identifier(number, row)
            if identifier in by_page[number]:
                vector = export_one(row, document, chosen[identifier])
                if vector: document['examples'].append(vector)
        frozen_vectors[number] = document
    # Fill any template/character case lost to raster rejection, preserving splits.
    existing = {r['id'] for r in records} | {r['id'] for r in raster_failures}
    for split in ['train', 'validation', 'test']:
        if split == 'train' and keep:
            continue
        missing = set(available[split])-set(sampled_usage[split])
        if not missing: continue
        candidates = [i for i in pools[split] if set(i['cases']) & missing and i['id'] not in existing]
        for item in candidates:
            if not (set(item['cases']) & missing): continue
            number = item['page'];cache = json.loads((snapshot/'pages'/f'page{number:03}.json').read_text())
            if number not in frozen_vectors:
                raw = json.loads((NATIVE/'output/vector-review'/f'page{number:03}.raw.json').read_text())
                frozen_vectors[number] = dict(schemaVersion=1, page=pages[number]['id'], provenance='machine-weak',
                    sourceSha256=pages[number]['sha256'], sourceCacheSha256=cache['sourceFileSha256'], raster=raw['raster'],
                    pdfToRaster=raw['provenance']['pdfToRaster'], templateSha256=cache['letterFormsSha256'], cacheVersion=cache['version'], examples=[])
            row = next(r for r in cache['subwords'] if audit.identifier(number, r) == item['id'])
            vector = export_one(row, frozen_vectors[number], split)
            if vector:
                frozen_vectors[number]['examples'].append(vector);existing.add(item['id'])
                missing = set(available[split])-set(sampled_usage[split])
    if keep and {r['id']: r['maskSha256'] for r in records if r['group'] == 'weak_train'} != prior_targets:
        raise ValueError('Retained automatic masks differ from the original review')
    for record, labels, roles in human:
        if record['split'] == 'train' and not has_training_boundary(record):
            excluded['fewer-than-two-training-bases'] += 1
            continue
        save_record(record, labels, roles)
    decoder, tooth_statistics = tooth_policy(tooth_areas, records)
    coverage=[]
    for form in ['initial', 'medial', 'final']:
        for rule in config[form]:
            for character in rule['current']:
                key=f"{rule['id']}|{form}|{character}"
                counts={s:available[s][key] for s in ['train','validation','test']}
                accepted={s:sampled_usage[s][key] for s in ['train','validation','test']}
                status='covered' if accepted['train'] else 'no-complete-training-occurrence' if not counts['train'] else 'no-raster-valid-training-occurrence'
                coverage.append(dict(template=rule['id'],position=form,character=character,availableComplete=counts,selected=accepted,status=status))
    rejected_ids = {r['id'] for r in raster_failures}
    eligible_cases = {key for item in pools['train'] if item['id'] not in rejected_ids for key in item['cases']}
    unrepresented = eligible_cases - set(sampled_usage['train']) - {key for key in eligible_cases if key.startswith('isolated|')}
    if unrepresented:
        raise ValueError('Eligible template cases remain uncovered: ' + ', '.join(sorted(unrepresented)))
    rules_without_training=sorted({r['id'] for f in ['initial','medial','final'] for r in config[f]}-
                                  {c['template'] for c in coverage if c['selected']['train']})
    groups = Counter(r['group'] for r in records)
    for number, document in frozen_vectors.items(): write(folder/'vector'/f'page{number:03}.json', document)
    manifest=dict(schemaVersion=2, seed=63, splitPolicy=corpus['splitPolicy'],
        input='Binary isolated subword ink only; no transcript, expected count, templates or cuts enter the model',
        target='Letter body and kashida instances; letter identity is not trained', roles={'0':'background','1':'body','2':'kashida'},
        tuningGroup='weak_validation', reviewedEvaluationGroup=None if args.cutter_only else 'human_validation',
        testUse='Evaluation and descriptive reporting only; no fitting or selection',
        minimumTrainingBaseCharacters=MIN_TRAINING_BASES,
        groups=dict(groups), records=sorted(records,key=lambda r:r['id']), decoder=decoder,
        sampling=dict(coverEveryTrainingExample=True,method='template/position/character coverage plus text/aspect diversity; shuffled passes during training',trainingPool=len(pools['train'])),
        includeProvisional=args.include_provisional,
        pageSplitOverrides=split_overrides,
        vectorSnapshotHashes={f'page{n:03}.json':sha(folder/'vector'/f'page{n:03}.json') for n in frozen_vectors})
    if args.cutter_only:
        manifest['excludedReviewedIds'] = sorted(reviewed_ids)
    if keep:
        manifest['sampling']['retainedFromDatasetSha256'] = sha(keep/'dataset.json')
    if args.review_training:
        manifest['trainingReview'] = {'status': 'open'}
    usage = []
    for template in sorted(set().union(*(set(counter) for counter in used_templates.values()))):
        key = f"{template}|used|*"
        usage.append(dict(template=template,
                          used={split:used_templates[split][template] for split in ('train','validation','test')},
                          selected={split:sampled_usage[split][key] for split in ('train','validation','test')}))
    write(folder/'dataset.json',manifest)
    write(folder/'template-coverage.json',dict(cases=coverage,usedTemplates=usage,
          rulesWithoutCompleteTrainingExample=rules_without_training,rasterRejected=raster_failures))
    write(folder/'tooth-area.json',dict(**decoder,analyzerAreaStatistics=tooth_statistics,measurements=tooth_areas))
    write(folder/'unfinished-inventory.json',unfinished)
    write(folder/'inputs/snapshot.json',dict(directory=str(snapshot),nativeRoot=str(NATIVE),pageHashes=page_hashes,templateFileSha256=sha(snapshot/'letter-forms.json'),
          segmenterSha256=generation['segmenterSha256'],generationSha256=sha(snapshot/'generation.json'),corpusSha256=sha(args.data/'corpus.json')))
    write(folder/'preparation.json',dict(groups=dict(groups),rules=len({c['template'] for c in coverage}),
        rulesWithTraining=len({c['template'] for c in coverage if c['selected']['train']}),rulesWithoutCompleteTrainingExample=rules_without_training,
        selectedCases=sum(c['status']=='covered' for c in coverage),definedCases=len(coverage),
        unfinishedEntries=len(unfinished),nativeScopeSummary=dict(audit_totals),exclusions=dict(excluded),rasterRejected=len(raster_failures),decoder=decoder))
    shutil.copytree(args.pretrained,folder/'pretrained')
    for name in ['letter_bases.py','letter_retest.py','letter_retest_data.py','letter_template_repeat.py','letter_cut_pilot.py','letter_retest_report.py','letter_page_compare.py']:
        shutil.copyfile(ROOT/name,folder/'code'/name)
    (folder/'code/automatic').mkdir();shutil.copyfile(ROOT/'automatic/__init__.py',folder/'code/automatic/__init__.py');shutil.copyfile(ROOT/'automatic/common.py',folder/'code/automatic/common.py')
    write(folder/'inputs/code-hashes.json',{str(p.relative_to(folder/'code')):sha(p) for p in (folder/'code').rglob('*.py')})
    import platform
    from importlib.metadata import version
    write(folder/'runtime-versions.json',dict(python=sys.executable,pythonVersion=platform.python_version(),
        **{name:version(name) for name in ['torch','transformers','numpy','scipy','Pillow','scikit-image','PySide6']}))
    previous_initialization=args.pretrained.parent/'mask2former/initialization.json'
    previous_state=json.loads(previous_initialization.read_text())['initialStateSha256'] if previous_initialization.exists() else None
    label_recipe = ('Cutter-only targets, with the indexed reviewed crops excluded'
                    if args.cutter_only else '10 percent reviewed-example sampling')
    write(folder/'run.json',dict(initialization='pretrained',steps=1800,batch=4,validateEvery=300,
        previousInitialStateSha256=previous_state,
        datasetSha256=sha(folder/'dataset.json'),pretrainedFiles={p.name:sha(p) for p in (folder/'pretrained').iterdir() if p.is_file()},
        templateFileSha256=sha(folder/'inputs/letter-forms.json'),nativeExporterSha256=generation['exporterSha256'],
        nativeCutterSha256=generation['segmenterSha256'],snapshot=str(snapshot),
        recipeChanges=['Template/position/character-diverse completed targets across existing training pages',
                       'Shuffled coverage schedule; checkpoints eligible only after every selected training crop was sampled',
                       'Minimum predicted region area is 0.1 times the mean training-only tooth area'],
        unchangedRecipe=['Generic pretrained Mask2Former Swin-Tiny initialization and new optimizer','1800 updates, batch four, seed 63',
                         '160 by 224 image-only input','Page-held-out fitting and selection'],
        labelRecipe=label_recipe))
    print(json.dumps(json.loads((folder/'preparation.json').read_text()),ensure_ascii=False,indent=2),flush=True)


def colored_mask(ink, instances, roles):
    colors=np.array([(38,118,159),(157,72,129),(45,133,96),(182,99,43),(107,96,167),(86,125,43)],'uint8')
    pixels=np.full((*ink.shape,3),255,'uint8')
    pixels[ink & (instances==0)]=(159,125,185)
    for i in np.unique(instances[instances>0]):
        pixels[instances==i]=(229,182,61) if roles[i]==2 else colors[(int(i)-1)%len(colors)]
    return Image.fromarray(pixels)


def write_report_site(folder, rows, summary, coverage):
    output=folder/'unfinished-report'
    # Escape HTML-sensitive characters in the inline JSON, including source text.
    payload=json.dumps(rows,ensure_ascii=False,separators=(',',':')).replace('<','\\u003c')
    case_rows=[]
    for item in coverage['cases']:
        case_rows.append('<tr>'+''.join('<td>'+escape(str(v))+'</td>' for v in [item['template'],item['position'],item['character'],
            item['availableComplete']['train'],item['selected']['train'],item['status']])+'</tr>')
    prep=json.loads((folder/'preparation.json').read_text())
    settings=json.loads((folder/'mask2former/settings.json').read_text())
    decoder=settings['decoder']
    info=(f"{summary['entries']:,} unfinished entries across {summary['pages']} pages · "
          f"{summary['predicted']:,} inferred · {summary['countMatches']:,} body-count matches · "
          f"{summary['countDifferences']:,} body-count differences · {summary['unavailable']:,} unavailable")
    html='''<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mask2Former · unfinished subwords</title><style>
*{box-sizing:border-box}body{margin:0;background:#f3f5f7;color:#20323e;font:15px/1.5 system-ui,sans-serif}main{max-width:1450px;margin:auto;padding:28px}header,.panel{background:white;border:1px solid #dae1e6;border-radius:12px;padding:24px;margin-bottom:20px}h1{font-size:30px;margin:4px 0 12px}h2{font-size:20px}p{margin:8px 0}.eyebrow{font-size:12px;letter-spacing:.1em;color:#377566}.muted,small{color:#607480}.stats{display:flex;gap:24px;flex-wrap:wrap;margin:16px 0}.stats strong{font-size:24px;display:block}.filters{display:flex;flex-wrap:wrap;gap:12px;align-items:end}.filters label{display:flex;flex-direction:column;font-size:12px;gap:4px}input,select,button{font:inherit;padding:9px 12px;border:1px solid #cbd5dc;border-radius:6px;background:white;color:inherit}input{min-width:240px}button{cursor:pointer}button:disabled{opacity:.4}#cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(390px,1fr));gap:16px}.case{background:white;border:1px solid #d5dfe5;border-radius:10px;padding:18px}.case.diff{border-left:4px solid #ba6834}.case.match{border-left:4px solid #37876c}.case.unavailable{border-left:4px solid #9b758b}.location{font-size:12px;color:#607480}.arabic{font-size:27px;line-height:1.8}.word{font-size:19px}.count{display:inline-flex;gap:18px;margin:8px 0;font-size:14px}.count strong{font-size:25px}.images{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:12px 0}.images figure{margin:0;min-width:0}.images img{width:100%;height:150px;object-fit:contain;background:white;image-rendering:auto}.images figcaption{font-size:12px;color:#607480;text-align:center}.detail{font-size:13px}pre{font-size:12px;white-space:pre-wrap;overflow-wrap:anywhere;max-height:260px;overflow:auto}nav{display:flex;gap:12px;align-items:center;flex-wrap:wrap}a{color:#247563}.pager{display:flex;justify-content:space-between;align-items:center;margin:18px 0}.table{overflow:auto;max-height:500px}table{border-collapse:collapse;width:100%;font-size:13px}th,td{text-align:left;border-bottom:1px solid #e0e6ea;padding:7px;white-space:nowrap}th{position:sticky;top:0;background:#eef3f4}.legend{display:flex;gap:15px;font-size:12px}.legend i{display:inline-block;width:12px;height:12px;border-radius:2px;margin-right:4px}footer{font-size:12px;color:#607480;padding:18px 0}@media(max-width:500px){main{padding:12px}header,.panel{padding:16px}#cards{grid-template-columns:1fr}.filters>*{width:100%}input{min-width:0}}
</style></head><body><main><header><div class="eyebrow">MASK2FORMER · PRETRAINED REPEAT</div><h1>Unfinished subwords</h1><p>__INFO__</p>
<p>Counts compare <strong>predicted letter bodies</strong> with the expected base letters. Connecting ink is counted separately. Matching counts do not verify the boundaries.</p>
<div class="stats"><div><strong>__RULES__</strong>template rules represented in training</div><div><strong>__TRAIN__</strong>automatic training crops</div><div><strong>__MIN__ pixels</strong>minimum predicted region area</div></div>
<p class="muted">The cutoff is one tenth of the mean tooth area, measured on completed training examples only. Filtered regions do not delete source ink. Purple shows ink without an accepted assignment; gold shows connections.</p>
<nav><a href="../template-coverage.json">Template coverage</a><a href="../tooth-area.json">Tooth measurements</a><a href="items.json">All prediction counts</a><a href="summary.json">Run summary</a><a href="../mask2former/evaluation.json">Held-out evaluation</a></nav></header>
<details class="panel"><summary>Training coverage and settings</summary><p>Fresh generic pretrained weights, __STEPS__ updates, batch 4; the selected checkpoint is update __SELECTED__. Only complete, nonprovisional cuts enter the automatic training set. Page splits are preserved. The model receives binary source ink without text or expected counts.</p><p>Mean tooth area: __MEAN__ raster pixels at the reference scale; fraction: 0.1. The exact page transform adjusts the cutoff when needed.</p><div class="table"><table><thead><tr><th>Template</th><th>Position</th><th>Letter</th><th>Available complete training cases</th><th>Selected</th><th>Status</th></tr></thead><tbody>__COVERAGE__</tbody></table></div></details>
<section class="panel"><div class="filters"><label>Search<input id="search" placeholder="Subword, full word or page location"></label><label>Count comparison<select id="comparison"><option value="all">All unfinished subwords</option><option value="diff">Different body count</option><option value="match">Matching body count</option><option value="unavailable">Inference unavailable</option></select></label><label>Page<select id="page"><option value="all">All pages</option></select></label><label>Page split<select id="split"><option value="all">All splits</option><option>train</option><option>validation</option><option>test</option></select></label><label>Examples per page<select id="size"><option>24</option><option selected>48</option><option>96</option></select></label><label>Prediction view<select id="view"><option value="completed">Ink completed</option><option value="raw">Raw supported masks</option></select></label><button id="clear">Clear filters</button></div><div class="pager"><span id="count" aria-live="polite"></span><nav><button id="previous">Previous</button><span id="pageNumber"></span><button id="next">Next</button></nav></div><div class="legend"><span><i style="background:#26769f"></i>Body instances</span><span><i style="background:#e5b63d"></i>Connections</span><span><i style="background:#9f7db9"></i>Unknown ink</span></div></section>
<section id="cards"></section><footer>Machine predictions from a frozen template/source revision. Existing human annotations and source artwork were preserved. Unfinished subwords were not used as automatic training targets. Training-page predictions are identified; count agreement is a transcript consistency check, not measured segmentation accuracy.</footer>
<script id="data" type="application/json">__DATA__</script><script>
const items=JSON.parse(document.getElementById('data').textContent),$=id=>document.getElementById(id);let offset=0,filtered=[];const esc=v=>String(v??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));const norm=v=>v.normalize('NFD').replace(/[\\u064B-\\u065F\\u0670\\u06D6-\\u06ED]/g,'').toLowerCase();
for(const n of [...new Set(items.map(x=>x.page))].sort((a,b)=>a-b)){let o=document.createElement('option');o.value=n;o.textContent='Page '+n;$('page').append(o)}
function render(){const size=Number($('size').value),page=Math.floor(offset/size),shown=filtered.slice(offset,offset+size);$('count').textContent=filtered.length.toLocaleString()+' subwords'+(filtered.length?' · '+(offset+1)+'–'+Math.min(offset+size,filtered.length):'');$('pageNumber').textContent=(filtered.length?page+1:0)+' / '+Math.ceil(filtered.length/size);$('previous').disabled=offset===0;$('next').disabled=offset+size>=filtered.length;
$('cards').innerHTML=shown.map(x=>{const status=x.predictedBodyCount==null?'unavailable':x.predictedBodyCount===x.expectedBodyCount?'match':'diff';const view=$('view').value==='raw'?x.rawImage:x.predictionImage;return `<article class="case ${status}" id="${esc(x.id)}"><div class="location">${esc(x.id)} · ${esc(x.split)}${x.usedForTraining?' · reviewed training example':''}</div><bdi class="arabic" dir="rtl">${esc(x.text)}</bdi><div class="word">Word: <bdi dir="rtl">${esc(x.wordText)}</bdi></div><div class="count"><span>Predicted bodies <strong>${x.predictedBodyCount??'—'}</strong></span><span>Expected bodies <strong>${x.expectedBodyCount??'—'}</strong></span></div>${x.predictedBodyCount!=null?`<div class="detail">${x.predictedConnectionCount} connections · ${x.predictedRegionCount} total regions · ${x.discardedSmallRegions} tiny regions filtered${x.countDifference?' · count difference '+(x.countDifference>0?'+':'')+x.countDifference:''}</div>`:''}${x.sourceImage?`<div class="images"><figure><img loading="lazy" src="${esc(x.sourceImage)}" alt="Original subword ink"><figcaption>Original ink</figcaption></figure><figure><img loading="lazy" src="${esc(view)}" alt="Predicted letter regions"><figcaption>${$('view').value==='raw'?'Raw supported masks':'Predicted regions · ink completed'}</figcaption></figure></div>`:`<p>${esc(x.unavailableReason||'Source geometry unavailable')}</p>`}<div class="detail">Template state: ${esc(x.reason)}${x.provisional?' · provisional remainder':''}${x.rawUnknownPixels?' · '+x.rawUnknownPixels+' raw unassigned ink pixels':''}</div><details><summary>Letter diagnostics and provenance</summary><pre>${esc(JSON.stringify({letters:x.letters,minimumRegionAreaPixels:x.minimumRegionAreaPixels,sourceInkPixels:x.sourceInkPixels,completedUnknownPixels:x.completedUnknownPixels,inputHash:x.inputHash,unavailableReason:x.unavailableReason},null,2))}</pre></details></article>`}).join('')}
function filter(){const q=norm($('search').value),c=$('comparison').value;filtered=items.filter(x=>(!q||norm(x.id+' '+x.text+' '+x.wordText).includes(q))&&($('page').value==='all'||x.page===Number($('page').value))&&($('split').value==='all'||x.split===$('split').value)&&(c==='all'||c==='unavailable'&&x.predictedBodyCount==null||c==='match'&&x.predictedBodyCount!=null&&x.countDifference===0||c==='diff'&&x.predictedBodyCount!=null&&x.countDifference!==0));offset=0;render()}
for(const id of ['comparison','page','split','size'])$(id).addEventListener('change',filter);$('search').addEventListener('input',filter);$('view').addEventListener('change',render);$('previous').onclick=()=>{offset=Math.max(0,offset-Number($('size').value));render()};$('next').onclick=()=>{offset+=Number($('size').value);render()};$('clear').onclick=()=>{$('search').value='';for(const id of ['comparison','page','split'])$(id).value='all';filter()};filter();
</script></main></body></html>'''
    substitutions={'__INFO__':escape(info),'__RULES__':str(prep['rulesWithTraining'])+'/'+str(prep['rules']),
        '__TRAIN__':f"{prep['groups']['weak_train']:,}",'__MIN__':str(math.ceil(decoder['minimumRegionAreaPixels'])),
        '__STEPS__':str(settings['trainSteps']),'__SELECTED__':str(settings['step']),
        '__MEAN__':f"{decoder['meanToothRasterArea']:.2f}",'__COVERAGE__':''.join(case_rows),'__DATA__':payload}
    for key,value in substitutions.items():html=html.replace(key,value)
    (output/'index.html').write_text(html)


def report(args):
    import torch
    from letter_retest import model_for, native, inputs, mask_decode, minimum_region_area
    from letter_page_compare import source_crop, inference_example
    folder=args.folder.resolve();output=folder/'unfinished-report'
    output.mkdir(exist_ok=True);(output/'assets').mkdir(exist_ok=True);(output/'predictions').mkdir(exist_ok=True)
    origin=json.loads((folder/'inputs/snapshot.json').read_text());snapshot=Path(origin['directory']);native_root=Path(origin['nativeRoot'])
    inventory=json.loads((folder/'unfinished-inventory.json').read_text())
    manifest=json.loads((folder/'dataset.json').read_text());settings=json.loads((folder/'mask2former/settings.json').read_text())
    if settings['datasetSha256']!=sha(folder/'dataset.json'):raise ValueError('Changed dataset')
    checkpoint_sha=sha(folder/'mask2former/best.pt');decoder=settings['decoder']
    torch.set_num_threads(4);torch.manual_seed(63)
    device=torch.device(args.device);model=model_for('mask2former',str(folder/'pretrained'),device,
                                                     seen_body_class=settings.get('seenBodyClass', False),
                                                     geometry_input=settings.get('geometryInput', False))
    model.load_state_dict(torch.load(folder/'mask2former/best.pt',map_location='cpu',weights_only=True));model.eval()
    training_ids={r['id'] for r in manifest['records'] if r['split']=='train'}
    splits={int(p['id'][4:]):p['split'] for p in json.loads((folder/'inputs/corpus.json').read_text())['pages']}
    by_page=defaultdict(list)
    for item in inventory:by_page[item['page']].append(item)
    rows=[];start=time.monotonic();done=0
    with torch.inference_mode():
        for number in sorted(by_page):
            path=snapshot/'pages'/f'page{number:03}.json'
            if sha(path)!=origin['pageHashes'][str(number)]:raise ValueError(f'Changed frozen native page {number}')
            page=json.loads(path.read_text());native_rows={(r['line'],r['word'],r['subword']):r for r in page['subwords']}
            raw=json.loads((native_root/'output/vector-review'/f'page{number:03}.raw.json').read_text())
            if raw['provenance']['cacheSha256']!=page['sourceFileSha256']:raise ValueError('Changed source raster provenance')
            transform=raw['provenance']['pdfToRaster'];width,height=raw['raster']['width'],raw['raster']['height']
            cutoff=minimum_region_area(decoder,{'pdfToRaster':transform})
            for item in by_page[number]:
                row=dict(item,split=splits[number],usedForTraining=item['id'] in training_ids,
                         predictedBodyCount=None,predictedConnectionCount=None,predictedRegionCount=None,
                         expectedBodyCount=item['expectedBodyCount'] or None,minimumRegionAreaPixels=cutoff)
                source=native_rows.get((item['line'],item['word'],item['subword']))
                if not source or not source.get('sourceInk'):
                    row['unavailableReason']='Native decomposition did not provide source subword geometry'
                    rows.append(row);continue
                source_runs=raster_runs(source['sourceInk'],width,height,transform)
                if not source_runs:
                    row['unavailableReason']='No rasterized source ink';rows.append(row);continue
                ink,bbox=source_crop({'id':item['id'],'sourceRuns':source_runs},width)
                example=inference_example({'id':item['id']},ink)
                input_hash=hashlib.sha256(np.packbits(ink).tobytes()+str(ink.shape).encode()).hexdigest()
                key=hashlib.sha256(json.dumps(dict(input=input_hash,model=checkpoint_sha,cutoff=cutoff,threshold=settings['threshold'],
                    decoderCode=sha(ROOT/'letter_retest.py'),device=args.device),sort_keys=True).encode()).hexdigest()
                prediction_path=output/'predictions'/f'{key}.npz'
                if prediction_path.exists():
                    with np.load(prediction_path) as f:
                        prediction=f['instances'];roles=f['roles'];supported=f['raw'];unfiltered_count=int(f['unfilteredCount'])
                else:
                    result=model(pixel_values=inputs(example['canvas'][None],device))
                    probabilities=native(result.masks_queries_logits[0],example).sigmoid().cpu().numpy()
                    classes=result.class_queries_logits[0].softmax(-1).cpu().numpy()
                    prediction,roles,supported=mask_decode(probabilities,classes,ink,settings['threshold'],min_region_area=cutoff,
                        minimum_owned_fraction=decoder.get('minimumOwnedFraction', 0) if isinstance(decoder, dict) else 0)
                    unfiltered,_,_=mask_decode(probabilities,classes,ink,settings['threshold'],complete=False,min_region_area=1)
                    unfiltered_count=int(unfiltered.max())
                    # Identical local ink can share inference across locations.
                    # Page coordinates belong to each report row, not this cache.
                    np.savez_compressed(prediction_path,instances=prediction,roles=roles,raw=supported,sourceInk=ink,
                        localShape=np.array(ink.shape),unfilteredCount=np.array(unfiltered_count))
                if np.any((prediction>0)&~ink) or np.any((supported>0)&~ink):raise ValueError('Prediction invented ink')
                if prediction.any() and not np.array_equal(prediction>0,ink):raise ValueError('Completion lost ink')
                ids=np.unique(prediction[prediction>0]);bodies=int(sum(roles[i]==1 for i in ids));connections=int(sum(roles[i]==2 for i in ids))
                name=item['id'].replace(':','__')
                source_image='assets/'+name+'-source.png';prediction_image='assets/'+name+'-prediction.png';raw_image='assets/'+name+'-raw.png'
                Image.fromarray(np.where(ink,35,255).astype('uint8')).save(output/source_image)
                colored_mask(ink,prediction,roles).save(output/prediction_image);colored_mask(ink,supported,roles).save(output/raw_image)
                row.update(predictedBodyCount=bodies,predictedConnectionCount=connections,predictedRegionCount=len(ids),
                    countDifference=bodies-item['expectedBodyCount'],discardedSmallRegions=unfiltered_count-len(ids),
                    rawUnknownPixels=int((ink&(supported==0)).sum()),completedUnknownPixels=int((ink&(prediction==0)).sum()),
                    sourceInkPixels=int(ink.sum()),sourceImage=source_image,predictionImage=prediction_image,rawImage=raw_image,
                    letters=source.get('letters',[]),inputHash=input_hash,predictionFile=str(prediction_path.relative_to(output)),
                    sourceCacheSha256=page['sourceFileSha256'],pdfToRaster=transform,bbox=bbox)
                rows.append(row);done+=1
                if done%100==0:print(f'Predicted {done}/{len(inventory)} unfinished entries · {time.monotonic()-start:.1f}s',flush=True)
    predicted=[r for r in rows if r['predictedBodyCount'] is not None]
    summary=dict(entries=len(rows),predicted=len(predicted),unavailable=len(rows)-len(predicted),pages=len(by_page),
        auditedPages=len(origin['pageHashes']),
        countMatches=sum(r['countDifference']==0 for r in predicted),countDifferences=sum(r['countDifference']!=0 for r in predicted),
        undersegmented=sum(r['countDifference']<0 for r in predicted),oversegmented=sum(r['countDifference']>0 for r in predicted),
        discardedSmallRegions=sum(r['discardedSmallRegions'] for r in predicted),
        noAcceptedRegions=sum(r['predictedRegionCount']==0 for r in predicted),
        bySplit={s:dict(entries=sum(r['split']==s for r in rows),predicted=sum(r['split']==s for r in predicted),
            countMatches=sum(r['split']==s and r['countDifference']==0 for r in predicted)) for s in ['train','validation','test']},
        elapsedSeconds=time.monotonic()-start,checkpointSha256=checkpoint_sha,datasetSha256=settings['datasetSha256'],
        selectedStep=settings['step'],confidenceThreshold=settings['threshold'],decoder=decoder,
        interpretation='Expected segments are base-letter bodies. Connections are separate and their expected count is not inferred from the transcript. Count agreement does not prove boundary accuracy.',
        sourceInkConservationVerified=True,provenance='machine-predictions',humanReviewed=False)
    write(output/'items.json',rows);write(output/'summary.json',summary)
    samples=json.loads((folder/'mask2former/sample-counts-at-best.json').read_text())
    trained=Counter()
    for record in manifest['records']:
        if record['group']=='weak_train':
            if not samples.get(record['id']):raise ValueError('Selected checkpoint did not see every training crop')
            for case in record.get('templateCases',[]):trained[case]+=samples[record['id']]
    write(folder/'mask2former/training-coverage.json',dict(selectedCheckpointStep=settings['step'],
        automaticExamplesSeen=sum(r['group']=='weak_train' and samples.get(r['id'],0)>0 for r in manifest['records']),
        templateCaseSampleCounts=dict(trained)))
    coverage=json.loads((folder/'template-coverage.json').read_text())
    write_report_site(folder,rows,summary,coverage)
    print(json.dumps(summary,ensure_ascii=False,indent=2),flush=True)


def verify(args):
    folder=args.folder.resolve();manifest=json.loads((folder/'dataset.json').read_text())
    groups=defaultdict(set);training_ids=set()
    for record in manifest['records']:
        if sha(folder/record['file'])!=record['maskSha256']:raise ValueError('Changed mask '+record['id'])
        groups[record['split']].add(record['page'])
        if record['split']=='train':training_ids.add(record['id'])
        if record['provenance']=='machine-weak':
            allowed_statuses = {'matched', 'not-required'}
            if manifest.get('includeProvisional') and record.get('provisional'):
                allowed_statuses.add('remainder')
            if any(letter['status'] not in allowed_statuses for letter in record['letters']):
                raise ValueError('Unfinished target entered training/evaluation')
    if any(groups[a]&groups[b] for a,b in [('train','validation'),('train','test'),('validation','test')]):
        raise ValueError('Page split leakage')
    settings=json.loads((folder/'mask2former/settings.json').read_text())
    if sha(folder/'dataset.json')!=settings['datasetSha256']:raise ValueError('Changed training manifest')
    initialization=json.loads((folder/'mask2former/initialization.json').read_text())
    run=json.loads((folder/'run.json').read_text())
    if initialization['mode']!='pretrained' or not initialization['optimizerReset']:
        raise ValueError('Run did not start with fresh pretrained initialization')
    if initialization['pretrainedWeightsSha256']!=run['pretrainedFiles']['model.safetensors']:
        raise ValueError('Wrong generic pretrained weights')
    initial_state_matches=(not run.get('previousInitialStateSha256') or
                           initialization['initialStateSha256']==run['previousInitialStateSha256'])
    if not initial_state_matches:raise ValueError('Initial model state differs from the previous pretrained recipe')
    samples=json.loads((folder/'mask2former/sample-counts-at-best.json').read_text())
    automatic=[r for r in manifest['records'] if r['group']=='weak_train']
    if any(not samples.get(r['id']) for r in automatic):raise ValueError('Training coverage is incomplete')
    if 'excludedReviewedIds' in manifest:
        if (set(samples) != {r['id'] for r in automatic} or settings['humanSampleProbability'] != 0):
            raise ValueError('Cutter-only checkpoint used non-cutter draws')
    tooth=json.loads((folder/'tooth-area.json').read_text())
    if any(r['id'] not in training_ids for r in tooth['measurements']):raise ValueError('Tooth estimator used held-out data')
    if not math.isclose(np.mean([r['analyzerArea'] for r in tooth['measurements']]),tooth['meanToothAnalyzerArea']):raise ValueError('Incorrect tooth average')
    inventory=json.loads((folder/'unfinished-inventory.json').read_text())
    rows=json.loads((folder/'unfinished-report/items.json').read_text())
    if {r['id'] for r in inventory}!={r['id'] for r in rows} or len(rows)!=len(inventory):raise ValueError('Report omitted unfinished entries')
    verified=0
    for row in rows:
        if row['predictedBodyCount'] is None:continue
        with np.load(folder/'unfinished-report'/row['predictionFile']) as f:
            ink=f['sourceInk'];prediction=f['instances'];raw=f['raw'];roles=f['roles']
        if np.any((prediction>0)&~ink) or np.any((raw>0)&~ink):raise ValueError('Invented source ink')
        if prediction.any() and not np.array_equal(prediction>0,ink):raise ValueError('Lost source ink')
        for i in np.unique(raw[raw>0]):
            if (raw==i).sum()<row['minimumRegionAreaPixels']:raise ValueError('Tiny predicted region survived cutoff')
        ids=np.unique(prediction[prediction>0]);bodies=sum(roles[i]==1 for i in ids)
        if bodies!=row['predictedBodyCount'] or bodies-row['expectedBodyCount']!=row['countDifference']:raise ValueError('Incorrect report count')
        verified+=1
    result=dict(datasetMasksVerified=len(manifest['records']),pageSplitsDisjoint=True,
        pretrainedInitializationMatchesPrevious=initial_state_matches,newOptimizerVerified=True,
        automaticTargetsAllComplete=True,selectedCheckpointSawAllAutomaticExamples=True,
        toothEstimatorTrainingOnly=True,unfinishedInventoryComplete=True,predictionPartitionsVerified=verified,
        sourceInkPreserved=True,minimumAreaVerified=True)
    write(folder/'verification.json',result);print(json.dumps(result,indent=2),flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=['prepare','report','verify'])
    parser.add_argument('--folder',type=Path,required=True)
    parser.add_argument('--snapshot',type=Path)
    parser.add_argument('--data',type=Path,default=ROOT/'data')
    parser.add_argument('--pretrained',type=Path,default=ROOT/'data/automatic/experiments/letter-pretrained-repro-20261104/pretrained')
    parser.add_argument('--training-examples',type=int,default=4500)
    parser.add_argument('--include-provisional',action='store_true',
                        help='Include complete provisional partitions as automatic targets')
    parser.add_argument('--cover-used-templates',action='store_true',
                        help='Move whole unreviewed validation pages into training to cover rare used templates')
    parser.add_argument('--review-index',type=Path,help='Directory with the original reviewed training queue and validation IDs')
    parser.add_argument('--cutter-only',action='store_true',
                        help='Exclude the indexed reviewed crops from all dataset splits and use only cutter targets')
    parser.add_argument('--review-training',action='store_true',help='Prepare a candidate dataset for exclusion review before training')
    parser.add_argument('--retain-training-from',type=Path,help='Keep eligible masks from an existing open review of the same snapshot')
    parser.add_argument('--device',default='mps')
    args=parser.parse_args()
    if args.action=='prepare':prepare(args)
    elif args.action=='report':report(args)
    else:verify(args)


if __name__=='__main__':main()
