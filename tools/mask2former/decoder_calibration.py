"""Select competing-mask decoding on validation pages with frozen weights."""
from pathlib import Path
import shutil
import time

import torch

from comparison import checkpoint
from letter_bases import check_training_records
from letter_corpus_audit import write_json
from letter_retest import decoder_options, model_for
from letter_template_repeat import sha
from refine import calibrate, eligible_result, result_rank
from train import examples, focus_validation
from workspace import read


def calibrate_decoder(folder, previous, device_name):
    if folder.exists():
        raise ValueError(f'Choose a fresh calibration directory: {folder}')
    run, manifest = read(previous / 'run.json'), read(previous / 'dataset.json')
    weights, settings = checkpoint(previous)
    if sha(previous / 'dataset.json') != run['datasetSha256'] or settings['datasetSha256'] != run['datasetSha256']:
        raise ValueError('The frozen dataset changed')
    if sha(previous / 'inputs' / 'corpus.json') != run['corpusSha256']:
        raise ValueError('The frozen page split changed')
    check_training_records(manifest['records'])
    if not isinstance(settings.get('decoder'), dict):
        raise ValueError('Calibration requires a saved decoder policy')
    folder.mkdir(parents=True)
    for name in ('masks', 'inputs', 'pretrained'):
        shutil.copytree(previous / name, folder / name)
    shutil.copyfile(previous / 'dataset.json', folder / 'dataset.json')
    run.update(previous=str(previous), calibrationParent=str(previous), calibrationParentSha256=sha(weights))
    write_json(folder / 'run.json', run)
    destination = folder / 'mask2former'
    destination.mkdir()
    shutil.copyfile(weights, destination / 'best.pt')
    code = destination / 'code'
    code.mkdir()
    import letter_retest
    for source in (Path(__file__), Path(__file__).with_name('refine.py'), Path(letter_retest.__file__)):
        shutil.copyfile(source, code / source.name)

    torch.set_num_threads(4)
    device = torch.device(device_name)
    model = model_for('mask2former', str(folder / 'pretrained'), device,
                      seen_body_class=settings.get('seenBodyClass', False),
                      geometry_input=settings.get('geometryInput', False))
    model.load_state_dict(torch.load(weights, map_location='cpu', weights_only=True))
    excluded = set(manifest.get('excludedReviewedIds', [])) | set(manifest.get('excludedCropIds', []))
    records = [r for r in manifest['records'] if r['split'] == 'validation']
    items = list(focus_validation(folder, run, examples(folder, records), excluded))
    started = time.monotonic()
    candidates = calibrate(model, items, settings, device, thresholds=(.35, .65, .8),
                           owned_fractions=(0, .25, .5, .75))
    fraction = decoder_options(settings['decoder'])['minimum_owned_fraction']
    baseline = next(r for r in candidates if r['threshold'] == settings['threshold']
                    and r['minimumOwnedFraction'] == fraction)
    for row in candidates:
        row['eligible'] = eligible_result(row, baseline)
    selected = min((r for r in candidates if r['eligible']), key=result_rank)
    saved = dict(settings, checkpoint=str(folder / 'pretrained'), threshold=selected['threshold'],
                 decoder=dict(settings['decoder'], minimumOwnedFraction=selected['minimumOwnedFraction']),
                 selectionGroup='weak_validation', selectionTotals=selected['general'])
    write_json(destination / 'settings.json', saved)
    write_json(destination / 'selected.json', dict(weights='best.pt', settings=saved,
                                                  stage='decoder-calibration', parentStep=settings['step']))
    write_json(destination / 'decoder-calibration.json', dict(
        checkpointSha256=sha(weights), datasetSha256=run['datasetSha256'],
        selectionSplit='validation', baseline=baseline, candidates=candidates, selected=selected,
        elapsedSeconds=time.monotonic() - started,
    ))
    print(f"Selected confidence {selected['threshold']}, owned fraction {selected['minimumOwnedFraction']}: "
          f"validation F1 {selected['general']['roleAwareInstanceF1AtIoU50']:.2%}, "
          f"seen splits {selected['focus']['fragmentedSeenCrops']}, "
          f"extra seen fragments {selected['focus']['straySeenCrops']}.", flush=True)
