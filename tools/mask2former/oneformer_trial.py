#!/usr/bin/env python3
"""Compare pretrained OneFormer and Mask2Former on frozen letter segmentation data."""
import argparse
from collections import defaultdict
from functools import partial
import math
from pathlib import Path
import random
import time

from workspace import read, run_path

import torch
from transformers import CLIPTokenizer, OneFormerForUniversalSegmentation

from comparison import checkpoint
from letter_corpus_audit import batch_probabilities, write_json
from letter_retest import CANVAS, SEED, load_example, loss_for, model_for
from letter_template_repeat import sha
from transcript_input import evaluate_masks


def oneformer(checkpoint_path, device):
    labels = {0: 'body', 1: 'kashida'}
    model = OneFormerForUniversalSegmentation.from_pretrained(
        checkpoint_path, local_files_only=True, ignore_mismatched_sizes=True,
        num_labels=len(labels), id2label=labels, label2id={v: k for k, v in labels.items()},
        train_num_points=2048, use_auxiliary_loss=True, is_training=False,
    ).float().to(device)
    # This trial uses a single instance task and the mask and class losses.
    model.config.contrastive_temperature = None
    tokenizer = CLIPTokenizer.from_pretrained(checkpoint_path, local_files_only=True)
    tokens = tokenizer('the task is instance', padding='max_length', truncation=True,
                       max_length=model.config.task_seq_len, return_tensors='pt')
    task = (tokens.input_ids * tokens.attention_mask).to(device)

    def predict(*, pixel_values, **targets):
        return model(pixel_values=pixel_values, task_inputs=task.expand(len(pixel_values), -1),
                     **targets)

    return model, predict


def fit(model, predict, training, device, steps, name):
    backbone, head = [], []
    for parameter_name, parameter in model.named_parameters():
        group = backbone if parameter_name.startswith('model.pixel_level_module.encoder.') else head
        group.append(parameter)
    rates = (2e-5, 2e-4)
    optimizer = torch.optim.AdamW([
        dict(params=parameters, lr=rate) for parameters, rate in zip((backbone, head), rates)
    ], weight_decay=.01)
    sampling, augmentation = random.Random(SEED), random.Random(SEED)
    schedule = training.copy()
    sampling.shuffle(schedule)
    cursor = 0
    start = time.monotonic()
    model.train()
    for step in range(1, steps + 1):
        batch = []
        for _ in range(4):
            if cursor == len(schedule):
                sampling.shuffle(schedule)
                cursor = 0
            batch.append(schedule[cursor])
            cursor += 1
        ratio = min(1, step / 50) * (.1 + .9 * (1 + math.cos(math.pi * step / steps)) / 2)
        for group, rate in zip(optimizer.param_groups, rates):
            group['lr'] = rate * ratio
        optimizer.zero_grad(set_to_none=True)
        loss = loss_for(predict, 'mask2former', batch, device, augmentation)
        if not torch.isfinite(loss):
            raise ValueError(f'Nonfinite loss in {name} at step {step}')
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), .1, error_if_nonfinite=True)
        optimizer.step()
        if step == 1 or step % 30 == 0:
            print(f'{name} {step}/{steps}, loss {float(loss.detach()):.3f}, '
                  f'{time.monotonic() - start:.0f}s', flush=True)


def run(source, pretrained, output, steps, device):
    baseline_weights, settings = checkpoint(source)
    dataset = source / 'dataset.json'
    dataset_hash = sha(dataset)
    if (dataset_hash != settings['datasetSha256'] or settings['canvas'] != list(CANVAS)
            or settings.get('seenBodyClass') or settings.get('geometryInput')):
        raise ValueError('Use the frozen two-class 160 × 224 image-only baseline')
    records = read(dataset)['records']
    pages = defaultdict(set)
    for record in records:
        pages[record['page']].add(record['split'])
    if any(len(splits) != 1 for splits in pages.values()):
        raise ValueError('Page splits overlap')
    examples = [load_example(source, record) for record in records]
    training = [e for e in examples if e['record']['split'] == 'train']
    heldout = [e for e in examples if e['record']['split'] in {'validation', 'test'}]
    output.mkdir(parents=True, exist_ok=False)
    report = dict(source=str(source), datasetSha256=dataset_hash, canvas=list(CANVAS),
                  seed=SEED, steps=steps, batchSize=4, threshold=settings['threshold'],
                  trainingCrops=len(training), decoder=settings['decoder'], device=str(device),
                  reference='Held-out automatic cutter masks',
                  oneformerCheckpoint=str(pretrained),
                  oneformerPretrainedSha256=sha(pretrained / 'pytorch_model.bin'),
                  mask2formerPretrainedSha256=sha(source / 'pretrained/model.safetensors'),
                  savedMask2FormerSha256=sha(baseline_weights), results={})
    for name in ('oneformer', 'mask2former', 'saved-mask2former'):
        torch.manual_seed(SEED)
        if name == 'oneformer':
            model, predict = oneformer(pretrained, device)
        else:
            model = predict = model_for('mask2former', str(source / 'pretrained'), device)
        if name == 'saved-mask2former':
            model.load_state_dict(torch.load(baseline_weights, map_location='cpu', weights_only=True))
        else:
            fit(model, predict, training, device, steps, name)
            torch.save(model.state_dict(), output / f'{name}.pt')

        def predict_masks(_batch, items, threshold):
            return batch_probabilities(predict, items, device, threshold)

        details = {}
        result = evaluate_masks(model, predict_masks, heldout, settings, details)
        report['results'][name] = result
        write_json(output / f'{name}-test-counts.json', details)
        write_json(output / 'results.json', report)
        print(name, result['test/raw'], flush=True)
        del model, predict
        if device.type == 'mps':
            torch.mps.empty_cache()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--from-run', default='baseline-160x224')
    parser.add_argument('--pretrained', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--steps', type=int, default=360)
    parser.add_argument('--device', choices=('cpu', 'mps', 'cuda'), default='mps')
    args = parser.parse_args()
    if args.steps <= 0:
        parser.error('--steps must be positive')
    torch.set_num_threads(4)
    run(run_path(args.from_run), args.pretrained.resolve(), args.output.resolve(),
        args.steps, torch.device(args.device))


if __name__ == '__main__':
    main()
