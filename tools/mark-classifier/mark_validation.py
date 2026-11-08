"""Read-only validation of native C++ mark assignments and classifier identities."""
from collections import Counter
import hashlib
import json
import math


def digest_json(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                    separators=(',', ':')).encode()).hexdigest()


def painted_source_paths(assignment):
    """Read the native painted masks while retaining the two original contour masks on disk."""
    replacements = {}
    for sub in assignment['subwords']:
        body = sub.get('compoundBody')
        if body:
            replacements[body['primaryPathId']] = body
            replacements[body['secondaryPathId']] = {'runs': []}
    return [{'runs': replacements.get(p['id'], p)['runs']} for p in assignment['paths']]


def mask_union(parts):
    intervals = sorted((s, s+n) for p in parts for s, n in zip(p['runs'][::2], p['runs'][1::2]))
    merged = []
    for start, end in intervals:
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return [v for start, end in merged for v in (start, end-start)]


def ink_box(runs, width):
    left = width; right = 0; top = math.inf; bottom = 0
    for start, count in zip(runs[::2], runs[1::2]):
        while count:
            y, x = divmod(start, width); n = min(count, width-x)
            left = min(left, x); right = max(right, x+n); top = min(top, y); bottom = max(bottom, y+1)
            count -= n; start += n
    if top == math.inf:
        raise ValueError('Empty native ink mask')
    return [left, top, right-left, bottom-top]


def has_yeh_hamza_kasra(owner):
    return owner.get('character') == 'ئ' and '\u0650' in owner.get('text', '')


def normalize_contextual_hamza(expected, observed, positions):
    """Accept a below-form only for a missing hamza in the agreed ئِ context.

    This is validation after ownership. Preserve raw predictions, count every
    hamza, and never consume a below-form needed by a separate expected mark.
    """
    result = Counter(observed)
    count = min(positions, max(0, expected.get('hamza-above', 0)-result['hamza-above']),
                max(0, result['hamza-below']-expected.get('hamza-below', 0)))
    if count:
        result['hamza-above'] += count
        result['hamza-below'] -= count
        if not result['hamza-below']:
            del result['hamza-below']
    return result, count


def comparison_inventory(row):
    return Counter(row.get('comparisonClassified', row['classified']))


def validate_character_links(owner, classified, word):
    slots = {s['characterOffset']: s for s in owner['expectedSlots']}
    text = word['text']
    if len(slots) != len(owner['expectedSlots']):
        raise ValueError(f'{owner["id"]}: duplicate expected transcript character')
    if not 0 <= owner['offset'] < len(text) or text[owner['offset']] != owner['character']:
        raise ValueError(f'{owner["id"]}: owner differs from transcript text')
    for offset, slot in slots.items():
        if (not 0 <= offset < len(text) or text[offset] != slot['sourceCharacter']
                or slot['codePoint'] != f'U+{ord(text[offset]):04X}'):
            raise ValueError(f'{owner["id"]}: character offset differs from transcript text')
    linked, candidates = set(), set()
    for group, prediction in zip(owner['groups'], classified, strict=True):
        link = group.get('transcriptLink', {})
        status = link.get('status')
        letter = owner['role'] == 'letter'
        if (link.get('baseOffset') != (owner['offset'] if letter else None) or
                link.get('baseCharacter') != (owner['character'] if letter else None)):
            raise ValueError(f'{group["id"]}: transcript link has the wrong base')
        if status == 'linked':
            offset = link['characterOffset']
            slot = slots.get(offset)
            if (slot is None or offset in linked or link['characterId'] != f'{word["id"]}:c{offset}' or
                    any(link[key] != slot[key] for key in ('sourceCharacter', 'codePoint')) or
                link['expectedType'] != slot['type'] or group['contours'] != slot['capacity'] or
                link.get('equivalence') != slot.get('equivalence')):
                raise ValueError(f'{group["id"]}: inconsistent or duplicate transcript character link')
            linked.add(offset)
            expected = 'body-ink' if slot['type'] == 'small-kaf-body' else slot['type']
            observed = prediction['inventoryType']
            status = 'match' if observed == expected else 'different'
            if (has_yeh_hamza_kasra(owner) and slot['sourceCharacter'] == 'ئ' and
                    expected == 'hamza-above' and observed == 'hamza-below'):
                status = 'accepted-form'
            prediction['transcriptCharacterId'] = link['characterId']
        elif status in ('ambiguous', 'unmatched'):
            alternatives = link['candidateOffsets']
            if (bool(alternatives) != (status == 'ambiguous') or 'characterId' in link or
                    any(t not in slots or slots[t]['capacity'] != group['contours'] for t in alternatives)):
                raise ValueError(f'{group["id"]}: invalid transcript alternatives')
            candidates.update(alternatives)
        else:
            raise ValueError(f'{group["id"]}: missing native transcript character link')
        prediction['characterMatch'] = status
    if linked & candidates:
        raise ValueError(f'{owner["id"]}: ambiguous ink refers to an already linked character')
    return dict(characterLinksMatch=(len(linked) == len(slots) and
                all(p['characterMatch'] in ('match', 'accepted-form') for p in classified)),
                charactersWithoutInk=sorted(slots.keys() - linked - candidates))


def compare_predictions(assignment, predictions):
    """Read-only comparison: predictions cannot modify a path's frozen owner."""
    by_id = {p['id']: p for p in predictions}
    if len(by_id) != len(predictions) or set(by_id) != {g['id'] for g in assignment['groups']}:
        raise ValueError('Predictions must cover each frozen group exactly once')
    result = []
    direct_links = assignment.get('schemaVersion', 0) >= 5
    words = {word['id']: word for word in assignment['words']} if direct_links else {}
    for sub in assignment['subwords']:
        rows = []
        for owner in sub['owners']:
            observed = Counter(); classified = []
            for group in owner['groups']:
                prediction = by_id[group['id']]
                kind = prediction['kind']
                label = ('body-ink' if kind in ('subword-ink', 'body') else
                         'kashida' if kind == 'kashida' else
                         'other-ink' if kind == 'other' else prediction['type'])
                observed[label or 'unknown'] += 1
                classified.append(dict(groupId=group['id'], **prediction, inventoryType=label or 'unknown'))
            expected = Counter(owner['expectedInventory'])
            contextual = has_yeh_hamza_kasra(owner)
            comparable, accepted = normalize_contextual_hamza(expected, observed, int(contextual))
            missing = expected-comparable; extra = comparable-expected
            rows.append(dict(id=owner['id'], expected=dict(expected), classified=dict(observed),
                             missing=dict(missing), extra=dict(extra), predictions=classified,
                             matches=not missing and not extra,
                             contourMatches=owner['expectedContours'] == owner['assignedContours'],
                             groupingMatches=not owner['groupingIssues']))
            if contextual:
                rows[-1]['acceptedHamzaForms'] = ['hamza-above', 'hamza-below']
            if accepted:
                rows[-1].update(comparisonClassified=dict(comparable),
                    contextualEquivalences=[dict(rule='yeh-hamza-with-kasra',
                        observed='hamza-below', expected='hamza-above', count=accepted)])
            if direct_links:
                rows[-1].update(validate_character_links(owner, classified, words[sub['wordId']]))
        inventories_match = (all(r['matches'] and r['contourMatches'] and r['groupingMatches'] for r in rows)
                             and not sub['unusedPathIds'])
        result.append(dict(id=sub['id'], owners=rows, matches=inventories_match))
        if direct_links:
            character_links_match = all(r['characterLinksMatch'] for r in rows)
            result[-1].update(inventoryAndStructureMatch=inventories_match,
                              characterLinksMatch=character_links_match,
                              matches=inventories_match and character_links_match)
    return result
