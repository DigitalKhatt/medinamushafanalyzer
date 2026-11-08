#!/usr/bin/env python3
"""Repeat native Mask2Former training or launch the connected-crop comparison."""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import venv

from workspace import ENV, HERE, NATIVE, latest_report, read, run_path


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def setup():
    python = ENV / 'bin' / 'python'
    if not python.is_file():
        venv.EnvBuilder(with_pip=True).create(ENV)
    subprocess.run([str(python), '-m', 'pip', 'install', '--only-binary=:all:',
                    '-r', str(HERE / 'requirements.txt')], check=True)
    print(f'Mask2Former Python ready at {python}', flush=True)


def current_snapshot():
    engine = digest(NATIVE / 'segmentsubword.cpp')
    templates = digest(NATIVE / 'templates' / 'letter-forms.json')
    font = digest(NATIVE / 'input' / 'font.dat')
    for path in sorted((NATIVE / 'output').glob('*/generation.json'),
                       key=lambda file: file.stat().st_mtime, reverse=True):
        generation = read(path)
        if (generation.get('completedPages') == list(range(1, 605)) and not generation.get('failures')
                and generation.get('segmenterSha256') == engine and generation.get('templateFileSha256') == templates
                and generation.get('fontFileSha256') == font and not generation.get('trackerEnabled')
                and (path.parent / 'letter-forms.json').is_file()
                and all((path.parent / 'pages' / f'page{n:03}.json').is_file() for n in range(1, 605))):
            if all(generation['pageHashes'][str(n)]['source'] == digest(NATIVE / 'pages' / f'page{n}.dat')
                   for n in range(1, 605)):
                return path.parent
    return None


def export_snapshot(jobs, rebuild=False):
    if not rebuild and (snapshot := current_snapshot()):
        return snapshot
    if jobs <= 0:
        raise ValueError('Choose at least one native export worker')
    build = NATIVE / 'build' / 'ninja-nmc'
    if not (build / 'build-Release.ninja').is_file():
        subprocess.run(['cmake', '--preset', 'ninja-nmc'], cwd=NATIVE, check=True)
    subprocess.run(['cmake', '--build', '--preset', 'release', '--target', 'medinamushafanalyzer'],
                   cwd=NATIVE, check=True)
    sys.path.insert(0, str(NATIVE))
    from tools.build_native_exporter import build as build_exporter
    from tools.letter_audit.regenerate_letter_audit import run

    name = datetime.now().strftime('mask2former-native-%Y%m%d-%H%M%S-%f')
    snapshot = NATIVE / 'output' / name
    exporter = snapshot / 'exporter'
    build_exporter(exporter, 'export_incomplete_corpus.cpp')
    if run(snapshot, exporter, jobs=jobs):
        raise ValueError(f'Native export failed. Check {snapshot / "logs"}.')
    return snapshot


def ids(path):
    if path is None:
        return set()
    rows = read(path)
    if not isinstance(rows, list) or not all(isinstance(sid, str) for sid in rows):
        raise ValueError(f'Use a JSON array of subword IDs in {path}')
    return set(rows)


def resolve_device(value):
    import torch

    if value == 'auto':
        value = 'mps' if torch.backends.mps.is_available() else 'cuda' if torch.cuda.is_available() else 'cpu'
    if value == 'mps' and not torch.backends.mps.is_available():
        raise ValueError('Apple GPU unavailable in this process. Use --device cpu or start from VS Code.')
    if value == 'cuda' and not torch.cuda.is_available():
        raise ValueError('CUDA is unavailable in this process')
    print(f'Using {value} for Mask2Former.', flush=True)
    return value


def prepare_run(args, folder):
    from data import prepare

    if folder.exists():
        raise ValueError(f'Choose a fresh run name. Already exists: {folder}')
    if args.seen_body_baseline:
        if any((args.snapshot, args.pretrained, args.focus, args.exclude, args.include, args.add_test_focus,
                args.previous != 'latest')):
            raise ValueError('The seen-body trial reuses the baseline snapshot, crop selection and initial weights')
        previous = run_path(args.seen_body_baseline)
        manifest = read(previous / 'dataset.json')
        run = read(previous / 'run.json')
        if manifest.get('seenBodyClass') or digest(previous / 'dataset.json') != run['datasetSha256']:
            raise ValueError('Use an unchanged two-class dataset as the seen-body baseline')
        if not (previous / 'mask2former' / 'selected.json').is_file():
            raise ValueError('Finish the two-class baseline before creating the seen-body trial')
        folder.mkdir(parents=True)
        for name in ('masks', 'inputs', 'pretrained'):
            shutil.copytree(previous / name, folder / name)
        manifest['seenBodyClass'] = True
        dataset = folder / 'dataset.json'
        dataset.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
        run.update(previous=str(previous), datasetSha256=digest(dataset), seenBodyBaseline=str(previous))
        (folder / 'run.json').write_text(json.dumps(run, ensure_ascii=False, indent=2) + '\n')
        print(f'Frozen three-class targets use the same masks and page split as {previous}.', flush=True)
        return
    previous = latest_report() if args.previous == 'latest' else None if args.previous == 'none' else run_path(args.previous)
    pretrained = args.pretrained.resolve() if args.pretrained else (previous / 'pretrained' if previous else None)
    if pretrained is None or not all((pretrained / name).is_file() for name in ('config.json', 'model.safetensors')):
        raise ValueError('Set --pretrained to a local pretrained Mask2Former directory')
    snapshot = args.snapshot.resolve() if args.snapshot else export_snapshot(args.jobs)
    print(f'Native input {snapshot}', flush=True)
    prepare(folder, snapshot, pretrained, previous, ids(args.focus), ids(args.exclude), ids(args.include),
            ids(args.add_test_focus))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action', required=True)
    commands.add_parser('setup', help='Install the pinned Python dependencies into the project environment')
    native = commands.add_parser('snapshot', help='Prepare a frozen export of the current native cuts')
    native.add_argument('--rebuild', action='store_true')
    native.add_argument('--jobs', type=int, default=8)
    for name in ('prepare', 'train', 'repeat', 'refine', 'calibrate', 'resolution', 'grouping',
                 'geometry-input', 'compare', 'predict', 'serve'):
        cmd = commands.add_parser(name)
        cmd.add_argument('--run', default='latest' if name == 'serve' else None,
                         required=name != 'serve', help='Run name or absolute experiment directory')
        if name in ('train', 'repeat', 'refine', 'calibrate', 'resolution', 'grouping',
                    'geometry-input', 'compare', 'predict'):
            cmd.add_argument('--device', choices=('auto', 'mps', 'cuda', 'cpu'), default='auto')
        if name in ('prepare', 'repeat'):
            cmd.add_argument('--seen-body-baseline',
                             help='Frozen two-class run whose masks and splits to reuse for a seen-body trial')
            cmd.add_argument('--snapshot', type=Path)
            cmd.add_argument('--pretrained', type=Path)
            cmd.add_argument('--previous', default='latest', help='Previous run, latest, or none')
            cmd.add_argument('--focus', type=Path, help='JSON array of additional seen training crop IDs')
            cmd.add_argument('--include', type=Path, help='JSON array of complete cutter training crop IDs to include')
            cmd.add_argument('--add-test-focus', type=Path,
                             help='Add only these seen crops and retire their pages from the previous test split')
            cmd.add_argument('--exclude', type=Path, help='JSON array of crop IDs with unreliable cuts')
            cmd.add_argument('--jobs', type=int, default=8)
        if name in ('train', 'repeat'):
            cmd.add_argument('--steps', type=int, default=1800)
            cmd.add_argument('--validate-every', type=int, default=300)
            cmd.add_argument('--replay-steps', type=int, default=360)
            cmd.add_argument('--compare-previous', action='store_true',
                             help='After training, compare both checkpoints on the same held-out crops')
        if name in ('refine', 'calibrate', 'resolution', 'grouping', 'geometry-input'):
            cmd.add_argument('--from-run', required=True, help='Frozen trained run to continue')
        if name == 'refine':
            cmd.add_argument('--steps', type=int, default=2400)
            cmd.add_argument('--validate-every', type=int, default=400)
        if name == 'geometry-input':
            cmd.add_argument('--extractor', choices=('contour-v1', 'stroke-v2'), default='contour-v1')
        if name == 'predict':
            cmd.add_argument('--batch', type=int, default=32)
            cmd.add_argument('--pages', type=int, nargs='+', help='A small smoke pass saved separately')
        if name == 'serve':
            cmd.add_argument('--port', type=int, default=8793)
    args = parser.parse_args()

    if args.action == 'setup':
        setup()
        return
    if args.action == 'snapshot':
        print(export_snapshot(args.jobs, args.rebuild))
        return
    if args.action == 'serve':
        from comparison import serve

        folder = latest_report() if args.run == 'latest' else run_path(args.run)
        if folder is None:
            raise ValueError('Run prediction before opening the comparison')
        serve(folder, args.port)
        return
    if args.run == 'latest':
        raise ValueError('Give the run a distinct name')
    folder = run_path(args.run)
    if args.action in ('prepare', 'repeat'):
        prepare_run(args, folder)
    if args.action in ('train', 'repeat'):
        from train import evaluate, fit

        device = resolve_device(args.device)
        fit(folder, device, args.steps, args.validate_every, args.replay_steps)
        evaluate(folder, device)
        if args.compare_previous:
            from train import compare_heldout

            compare_heldout(folder, device)
    if args.action == 'compare':
        from train import compare_heldout

        compare_heldout(folder, resolve_device(args.device))
    if args.action == 'refine':
        from refine import refine
        from train import compare_heldout, evaluate

        device = resolve_device(args.device)
        refine(folder, run_path(args.from_run), device, args.steps, args.validate_every)
        evaluate(folder, device)
        compare_heldout(folder, device)
    if args.action == 'calibrate':
        from decoder_calibration import calibrate_decoder
        from train import compare_heldout, evaluate

        device = resolve_device(args.device)
        calibrate_decoder(folder, run_path(args.from_run), device)
        evaluate(folder, device)
        compare_heldout(folder, device)
    if args.action in ('resolution', 'grouping', 'geometry-input'):
        from comparison import checkpoint
        from letter_corpus_audit import write_json
        from train import compare_heldout, evaluate, fit

        previous = run_path(args.from_run)
        weights, settings = checkpoint(previous)
        if settings['initialization'] != 'pretrained' or settings['batchSize'] != 4 or settings.get('groupingWeight', 0):
            raise ValueError('Use a pretrained baseline with batch size 4 and standard losses')
        if args.action == 'resolution' and settings['canvas'] != [160, 224]:
            raise ValueError('Use the saved 160 × 224 baseline for the resolution trial')
        geometry_input = args.extractor if args.action == 'geometry-input' else False
        if geometry_input and (settings['canvas'] != [160, 224] or settings.get('geometryInput')):
            raise ValueError('Use the 160 × 224 baseline with ink inputs for the geometry trial')
        steps = settings['trainSteps']
        if settings['step'] != steps:
            raise ValueError('Use a baseline selected at the final training step')
        run = read(previous / 'run.json')
        folder.mkdir(parents=True)
        for name in ('masks', 'inputs', 'pretrained'):
            shutil.copytree(previous / name, folder / name)
        shutil.copyfile(previous / 'dataset.json', folder / 'dataset.json')
        canvas_shape = (320, 448) if args.action == 'resolution' else tuple(settings['canvas'])
        grouping_weight = 1 if args.action == 'grouping' else 0
        run.update(previous=str(previous), trialBaselineSha256=digest(weights),
                   controlledTrial=dict(canvas=canvas_shape, checkpointStep=steps, steps=steps,
                                        decoder=settings['decoder'], threshold=settings['threshold'],
                                        groupingWeight=grouping_weight, geometryInput=geometry_input))
        write_json(folder / 'run.json', run)
        device = resolve_device(args.device)
        fit(folder, device, steps, steps, 0, canvas_shape=canvas_shape,
            decoder=settings['decoder'], thresholds=(settings['threshold'],),
            grouping_weight=grouping_weight, geometry_input=geometry_input)
        evaluate(folder, device)
        compare_heldout(folder, device, geometry_cohort=geometry_input)
    if args.action == 'predict':
        from comparison import predict

        print(predict(folder, resolve_device(args.device), args.batch, args.pages))


if __name__ == '__main__':
    try:
        main()
    except (FileNotFoundError, ValueError) as error:
        raise SystemExit(str(error)) from error
