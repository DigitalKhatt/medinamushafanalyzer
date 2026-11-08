"""Freeze complete native seen/reh and neighboring-tooth masks for the two-role trial."""
from collections import Counter
from pathlib import Path
import argparse
import json
import re
import numpy as np

from letter_retest_data import weak_partition
from letter_bases import MIN_TRAINING_BASES, base_character_count
from letter_template_repeat import complete_row, sha, vector_row, write


def read(path):
    return json.loads(path.read_bytes())


def prepare(baseline, snapshot, output):
    baseline, snapshot, output = (path.resolve() for path in (baseline, snapshot, output))
    if (output/'dataset.json').exists():
        raise ValueError('Dataset already frozen')
    generation = read(snapshot/'generation.json')
    partitions = read(snapshot/'ink-partition.json')
    if (generation['pagesCompleted'] != 604 or generation['failures'] or generation['trackerEnabled']
            or partitions['generationSha256'] != sha(snapshot/'generation.json')):
        raise ValueError('Need a complete tracker-off native audit and its ink partition checks')
    rejected_ids = {row['id'] for row in partitions['failures']}
    old = read(baseline/'dataset.json')
    reviewed = {r['id'] for r in old['records'] if r['group'].startswith('human_')}
    previous = read(baseline/'unfinished-report/items.json')
    hard = {r['id']:r for r in previous if r['countDifference'] > 0 and re.search('[سش]', r['baseText'])}
    index = read(baseline/'inputs/corpus.json')
    records, rejected, incomplete = [], [], []
    (output/'masks').mkdir(parents=True, exist_ok=True)
    for p in index['pages']:
        page = p['id']
        cache = read(snapshot/'pages'/(page+'.json'))
        ref = read(baseline/'vector'/(page+'.json'))
        if ref['sourceSha256'] != p['sha256'] or ref['sourceCacheSha256'] != cache['sourceFileSha256']:
            raise ValueError(f'Page source differs: {page}')
        for row in cache['subwords']:
            text = row['baseText']
            sid = f"{page}:l{row['line']}-w{row['word']}-s{row['subword']}"
            seen_reh = bool(re.search('[سش]ر', text))
            neighbor = bool(re.search('[بتثنيئى][سش]', text))
            if not (seen_reh or neighbor or sid in hard) or sid in reviewed:
                continue
            if p['split'] == 'train' and base_character_count(text) < MIN_TRAINING_BASES:
                rejected.append(dict(id=sid, reason='fewer-than-two-training-bases'))
                continue
            if not complete_row(row, rejected_ids, sid):
                incomplete.append(sid)
                continue
            if [body['letter'] for body in row['bodies']] != list(text):
                raise ValueError(f'Body identity/order mismatch: {sid}')
            v = vector_row(row, ref)
            try:
                labels, roles, box, repaired, clipped = weak_partition(v, ref['raster']['width'])
            except ValueError as error:
                rejected.append(dict(id=sid, reason=str(error)))
                continue
            if sum(roles == 1) != len(text):
                raise ValueError(f'Wrong body count: {sid}')
            if sid in hard:
                archive = baseline/'unfinished-report'/hard[sid]['predictionFile']
                with np.load(archive) as f:
                    if not np.array_equal(labels > 0, f['sourceInk']):
                        raise ValueError(f'Changed input for frozen failure: {sid}')
            path = Path('masks')/(sid.replace(':','__')+'.npz')
            np.savez_compressed(output/path, instances=labels, roles=roles)
            records.append(dict(id=sid, page=page, split=p['split'], group='weak_'+p['split'],
                provenance='machine-weak', text=text, textForDisplayOnly=row['text'], seenReh=seen_reh,
                separateTooth=neighbor, previouslyOvercounted=sid in hard,
                sourceSha256=p['sha256'], sourceCacheSha256=cache['sourceFileSha256'],
                templateSha256=cache['letterFormsSha256'], templates=row['templates'], bbox=box,
                pdfToRaster=ref['pdfToRaster'], repairedPixels=repaired, clippedExtraPixels=clipped,
                maskSha256=sha(output/path), file=str(path)))
    records.sort(key=lambda r:r['id'])
    # An outdated replay mask must not bypass the geometry or raster checks.
    replay_exclusions = set(incomplete) | {r['id'] for r in rejected}
    changes = snapshot/'changed-cuts.json'
    if changes.exists():
        replay_exclusions.update(read(changes)['changedPreviouslyComplete'])
    write(output/'dataset.json', dict(records=records, decoder=old['decoder'],
          frozenOriginalDataset=sha(baseline/'dataset.json'), sourceGeneration=sha(snapshot/'generation.json'),
          nativeSnapshot=str(snapshot), frozenCorpus=sha(baseline/'inputs/corpus.json'),
          groups=dict(Counter(r['split'] for r in records)), incomplete=incomplete,
          rasterRejected=rejected, excludedFromOldReplay=sorted(replay_exclusions), heldOutCountPanel=list(hard)))
    print('Frozen targets', dict(Counter(r['split'] for r in records)), 'partial',len(incomplete),
          'raster rejections',len(rejected),flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline','snapshot','output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    prepare(args.baseline,args.snapshot,args.output)
