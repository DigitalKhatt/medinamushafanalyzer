#!/usr/bin/env python3
"""Compare image, count and transcript inputs to the same Mask2Former checkpoint."""
import argparse
from collections import Counter, defaultdict
from functools import partial
from pathlib import Path
import random

from workspace import read, run_path

import numpy as np
import torch
from torch import nn

from comparison import checkpoint
from letter_corpus_audit import batch_probabilities, write_json
from letter_retest import (CANVAS, SEED, augmented_labels, decoder_options, inputs,
                           load_example, loss_for, mask_decode, metric, model_for, native, summarize)
from letter_template_repeat import sha
from seen import layout, measure, summarize as summarize_seen


class TranscriptQueries(nn.Module):
    """Add the ordered transcript to the learned query features before decoding."""

    def __init__(self, model, vocabulary_size, length):
        super().__init__()
        self.model = model
        self.length = length
        hidden = model.config.hidden_dim
        self.characters = nn.Embedding(vocabulary_size, 32, padding_idx=0)
        self.sequence_projection = nn.Linear(length * 32, hidden, bias=False)
        self.letter_projection = nn.Linear(32, hidden, bias=False)
        nn.init.zeros_(self.sequence_projection.weight)
        nn.init.zeros_(self.letter_projection.weight)

    def forward(self, *, transcript, **kwargs):
        encoded = self.characters(transcript)
        sequence = self.sequence_projection(encoded.flatten(1))
        letters = self.letter_projection(encoded).transpose(0, 1)

        def add_transcript(_module, args, options):
            queries = options['inputs_embeds'] + sequence.unsqueeze(0)
            options['inputs_embeds'] = torch.cat(
                [queries[:self.length] + letters, queries[self.length:]], dim=0)
            return args, options

        decoder = self.model.model.transformer_module.decoder
        handle = decoder.register_forward_pre_hook(add_transcript, with_kwargs=True)
        try:
            return self.model(**kwargs)
        finally:
            handle.remove()


class LetterSlotMatcher(nn.Module):
    """Fix body matches to transcript positions and match joins on free queries."""

    def __init__(self, join_matcher, target_slots, reserved_queries):
        super().__init__()
        self.join_matcher = join_matcher
        self.target_slots = target_slots
        self.reserved_queries = reserved_queries

    @torch.no_grad()
    def forward(self, masks, classes, mask_labels, class_labels):
        matches = []
        for i, slots in enumerate(self.target_slots):
            bodies = torch.where(slots >= 0)[0]
            joins = torch.where(slots < 0)[0]
            query_ids = slots[bodies]
            target_ids = bodies
            if joins.numel():
                (join_queries, join_targets), = self.join_matcher(
                    masks[i:i + 1, self.reserved_queries:],
                    classes[i:i + 1, self.reserved_queries:],
                    [mask_labels[i][joins]], [class_labels[i][joins]],
                )
                query_ids = torch.cat([query_ids, join_queries + self.reserved_queries])
                target_ids = torch.cat([target_ids, joins[join_targets]])
            matches.append((query_ids, target_ids))
        return matches


def dedicated_loss(model, batch, tokens, device, rng):
    labels = augmented_labels(batch, device, rng)
    targets, classes, slots = [], [], []
    for label, example in zip(labels, batch, strict=True):
        ids = label.unique()
        ids = ids[ids > 0]
        roles = torch.as_tensor(example['roles'], device=device)[ids]
        targets.append((label[None] == ids[:, None, None]).float())
        classes.append(roles.long() - 1)
        # Keep the original position when raster augmentation erases a tiny body.
        slots.append(torch.where(roles == 1, ids - 1, -1).cpu())

    criterion = model.model.criterion
    matcher = criterion.matcher
    criterion.matcher = LetterSlotMatcher(matcher, slots, model.length)
    try:
        return model(transcript=tokens, pixel_values=inputs(labels, device),
                     mask_labels=targets, class_labels=classes).loss
    finally:
        criterion.matcher = matcher


def encode(examples, mode, alphabet, length, device):
    tokens = torch.zeros((len(examples), length), dtype=torch.long)
    for i, example in enumerate(examples):
        text = [letter['character'] for letter in example['record']['letters']]
        if mode == 'count':
            tokens[i, :len(text)] = 2
        elif mode in {'transcript', 'dedicated', 'reversed'}:
            if mode == 'reversed':
                text.reverse()
            tokens[i, :len(text)] = torch.tensor([alphabet.get(letter, 1) for letter in text])
    return tokens.to(device)


def adjacent_seen_indices(letters):
    seen_letters, _ = layout(letters)
    indices = set()
    for letter in seen_letters:
        index = letter['letterIndex']
        for neighbor in letters[max(0, index - 2):index + 1]:
            neighbor_index = neighbor['letterIndex']
            if (abs(neighbor_index - index) == 1 and neighbor['character'] in 'بتثنيئى'
                    and (neighbor.get('template') or '').startswith('beh-')
                    and 'stacked' not in neighbor['template']):
                indices.update((index, neighbor_index))
    return indices


def adjacent_seen(letters, target, predicted, roles):
    """Score each sīn/shīn and adjacent tooth letter on the same crop."""
    indices = adjacent_seen_indices(letters)
    if not indices:
        return None
    matched = []
    for index in sorted(indices):
        reference = target == index
        candidates = [(np.count_nonzero(reference & (predicted == j)) /
                       max(1, np.count_nonzero(reference | (predicted == j))), j)
                      for j in np.flatnonzero(roles == 1)]
        matched.append(max(candidates, default=(0, 0)))
    return (all(iou >= .75 for iou, _ in matched)
            and len({region for _, region in matched}) == len(indices))


@torch.inference_mode()
def evaluate(model, mode, examples, settings, alphabet, length, device):
    def predict(batch, items, threshold):
        tokens = encode(batch, mode, alphabet, length, device)
        return batch_probabilities(partial(model, transcript=tokens), items, device, threshold)

    return evaluate_masks(model, predict, examples, settings)


@torch.inference_mode()
def evaluate_masks(model, predict, examples, settings, details=None):
    model.eval()
    threshold = settings['threshold']
    groups = defaultdict(list)
    focused = defaultdict(list)
    adjacent = defaultdict(list)
    for start in range(0, len(examples), 16):
        batch = examples[start:start + 16]
        items = [dict(example=e, ink=e['labels'] > 0) for e in batch]
        probabilities = predict(batch, items, threshold)
        for item, (masks, classes) in zip(items, probabilities, strict=True):
            example = item['example']
            record = example['record']
            prediction, roles, raw = mask_decode(masks, classes, item['ink'], threshold,
                **decoder_options(settings.get('decoder') or 3, record))
            for stage, regions in [('raw', raw), ('completed', prediction)]:
                key = f"{record['split']}/{stage}"
                scores = metric(example['labels'], regions, example['roles'], roles)
                groups[key].append(scores)
                seen = measure(record['letters'], example['labels'], regions, roles)
                if seen is not None:
                    focused[key].append(seen)
                correct = adjacent_seen(record['letters'], example['labels'], regions, roles)
                if correct is not None:
                    adjacent[key].append(correct)
                if details is not None and record['split'] == 'test':
                    details.setdefault(record['id'], {})[stage] = dict(
                        expected=scores['referenceRegions'], predicted=scores['predictedRegions'],
                        seenAndAdjacentToothCorrect=correct,
                    )
    return {key: dict(masks=summarize(rows), seen=summarize_seen(focused[key]),
                      seenAndAdjacentToothCrops=len(adjacent[key]),
                      seenAndAdjacentToothCorrect=sum(adjacent[key]))
            for key, rows in groups.items()}


@torch.inference_mode()
def audit_letter_slots(model, examples, settings, alphabet, length, device, mode='dedicated'):
    """Measure the mask at each intended letter position before resolving overlaps."""
    model.eval()
    totals = Counter()
    for start in range(0, len(examples), 16):
        batch = examples[start:start + 16]
        images = inputs(torch.stack([e['canvas'] for e in batch]), device)
        tokens = encode(batch, mode, alphabet, length, device)
        output = model(transcript=tokens, pixel_values=images)
        probabilities = output.class_queries_logits.softmax(-1).cpu().numpy()
        for i, example in enumerate(batch):
            letters = example['record']['letters']
            target = example['labels']
            count = len(letters)
            masks = (native(output.masks_queries_logits[i, :count], example) >= 0).cpu().numpy()
            masks &= target > 0
            references = target == np.arange(1, count + 1)[:, None, None]
            overlap = (masks & references).sum(axis=(1, 2))
            union = (masks | references).sum(axis=(1, 2))
            correct = overlap / np.maximum(1, union) >= .75
            scores = probabilities[i, :count]
            accepted = (scores.argmax(axis=1) == 0) & (scores[:, 0] >= settings['threshold'])
            totals['crops'] += 1
            totals['letterSlots'] += count
            totals['matchingLetterSlots'] += int(correct.sum())
            totals['matchingAcceptedLetterSlots'] += int((correct & accepted).sum())
            if indices := adjacent_seen_indices(letters):
                indices = np.array(sorted(indices)) - 1
                totals['seenAndAdjacentToothCrops'] += 1
                totals['seenAndAdjacentToothMatchingSlots'] += int(correct[indices].all())
                totals['seenAndAdjacentToothAcceptedSlots'] += int((correct & accepted)[indices].all())
    return dict(totals)


def run(source, output, device, steps, modes):
    weights, settings = checkpoint(source)
    dataset = source / 'dataset.json'
    if (sha(dataset) != settings['datasetSha256'] or settings['canvas'] != list(CANVAS)
            or settings.get('seenBodyClass') or settings.get('geometryInput')):
        raise ValueError('Use the frozen two-class 160 × 224 image-only baseline')
    records = read(dataset)['records']
    if any(not record.get('letters') for record in records):
        raise ValueError('Every crop needs an ordered transcript')
    pages = defaultdict(set)
    for record in records:
        pages[record['split']].add(record['page'])
    if (pages['train'] & (pages['validation'] | pages['test'])
            or pages['validation'] & pages['test']):
        raise ValueError('Page splits overlap')
    examples = [load_example(source, record) for record in records]
    if 'dedicated' in modes:
        for example in examples:
            record = example['record']
            body_ids = np.flatnonzero(example['roles'] == 1).tolist()
            indices = [letter['letterIndex'] for letter in record['letters']]
            if indices != body_ids or indices != list(range(1, len(indices) + 1)):
                raise ValueError(f"Body masks do not follow transcript order for {record['id']}")
    fitting = [e for e in examples if e['record']['split'] == 'train']
    heldout = [e for e in examples if e['record']['split'] in {'validation', 'test'}]
    focus = [e for e in fitting if any(layout(e['record']['letters']))]
    if not focus:
        raise ValueError('The training set needs seen or tooth examples')
    alphabet = {c: i + 3 for i, c in enumerate(sorted(
        {letter['character'] for e in fitting for letter in e['record']['letters']}))}
    length = max(len(e['record']['letters']) for e in fitting)
    if any(len(e['record']['letters']) > length for e in heldout):
        raise ValueError('A held-out transcript exceeds the training sequence capacity')
    output.mkdir(parents=True, exist_ok=False)
    report = dict(source=str(source), checkpointSha256=sha(weights), datasetSha256=sha(dataset),
                  steps=steps, batchSize=4, focusDrawsPerBatch=2, seed=SEED,
                  frozenImageFeatures=True, threshold=settings['threshold'],
                  decoder=settings.get('decoder'), alphabet=alphabet, sequenceCapacity=length,
                  reference='Held-out automatic cutter masks', results={})
    print(f'{len(fitting)} training crops, {len(focus)} focus crops, {len(heldout)} held-out crops.', flush=True)
    for mode in modes:
        torch.manual_seed(SEED)
        base = model_for('mask2former', str(source / 'pretrained'), device)
        base.load_state_dict(torch.load(weights, map_location='cpu', weights_only=True))
        base.model.pixel_level_module.requires_grad_(False)
        if length > base.config.num_queries:
            raise ValueError('The transcript exceeds the number of mask queries')
        if mode == 'dedicated' and length == base.config.num_queries:
            raise ValueError('Dedicated letter slots must leave queries available for joins')
        model = TranscriptQueries(base, len(alphabet) + 3, length).to(device)
        optimizer = torch.optim.AdamW([
            dict(params=[p for p in base.parameters() if p.requires_grad], lr=5e-5),
            dict(params=[p for name, p in model.named_parameters() if not name.startswith('model.')], lr=2e-4),
        ], weight_decay=.01)
        sampling, augmentation = random.Random(SEED), random.Random(SEED)
        torch.manual_seed(SEED)
        model.train()
        base.model.pixel_level_module.eval()
        for step in range(1, steps + 1):
            batch = sampling.sample(focus, 2) + sampling.sample(fitting, 2)
            tokens = encode(batch, mode, alphabet, length, device)
            optimizer.zero_grad(set_to_none=True)
            if mode == 'dedicated':
                loss = dedicated_loss(model, batch, tokens, device, augmentation)
            else:
                loss = loss_for(partial(model, transcript=tokens), 'mask2former', batch, device, augmentation)
            if not torch.isfinite(loss):
                raise ValueError(f'Nonfinite loss in {mode} at step {step}')
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), .1)
            optimizer.step()
            if step == 1 or step % 60 == 0:
                print(f'{mode} {step}/{steps}, loss {float(loss.detach()):.3f}', flush=True)
        result = evaluate(model, mode, heldout, settings, alphabet, length, device)
        report['results'][mode] = result
        if mode == 'dedicated':
            trainable = {name: value.detach().cpu()
                         for name, value in model.named_parameters() if value.requires_grad}
            torch.save(trainable, output / 'dedicated-trainable.pt')
            report['dedicatedQueryLayout'] = dict(
                bodyQueryStart=0, reservedBodyQueries=length, joinQueryStart=length,
                weights='dedicated-trainable.pt',
            )
            test = [example for example in heldout if example['record']['split'] == 'test']
            report['letterSlotAudit'] = audit_letter_slots(model, test, settings, alphabet, length, device)
        if mode == 'transcript':
            # Same letters and count, different order, on the same crops.
            reversible = []
            for example in heldout:
                text = [letter['character'] for letter in example['record']['letters']]
                if text != text[::-1]:
                    reversible.append(example)
            report['orderCheck'] = {
                ordering: evaluate(model, ordering, reversible, settings, alphabet, length, device)
                for ordering in ('transcript', 'reversed')
            }
        write_json(output / 'results.json', report)
        print(mode, result['test/raw'], flush=True)
        del model, base, optimizer
        if device.type == 'mps':
            torch.mps.empty_cache()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--from-run', default='baseline-160x224')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--steps', type=int, default=360)
    parser.add_argument('--device', default='mps')
    parser.add_argument('--modes', nargs='+', choices=['image', 'count', 'transcript', 'dedicated'],
                        default=['image', 'count', 'transcript'])
    args = parser.parse_args()
    if args.steps <= 0:
        parser.error('--steps must be positive')
    torch.set_num_threads(4)
    run(run_path(args.from_run), args.output.resolve(), torch.device(args.device), args.steps, args.modes)


if __name__ == '__main__':
    main()
