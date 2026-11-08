"""Post-inference transcript alignment. Never an input to Mask2Former."""
from letter_bases import BODY_LETTERS
CLASSES = dict((c, name) for name, chars in {
    'alef': 'اأإآٱ', 'beh': 'بتث', 'hah': 'جحخ', 'dal': 'دذ', 'reh': 'رز',
    'seen': 'سش', 'sad': 'صض', 'tah': 'طظ', 'ain': 'عغ', 'feh': 'ف', 'qaf': 'ق',
    'kaf': 'ك', 'lam': 'ل', 'meem': 'م', 'noon': 'ن', 'heh': 'هة',
    'waw': 'وؤ', 'yeh': 'يىئ', 'hamza': 'ء'}.items() for c in chars)


def letter_records(subword, characters, word_offset, word_ordinal):
    result, owners = [], {}
    previous = None
    for offset, char in enumerate(characters):
        base, form = char['text'], char['position']
        if base in BODY_LETTERS:
            ordinal = len(result) + 1
            previous = f"{subword['id']}-letter{ordinal}"
            result.append(dict(id=previous, subword_id=subword['id'], word_id=subword['word_id'],
                page_id=subword['page_id'], ordinal=ordinal, word_ordinal=word_ordinal+ordinal,
                char=base, form=form, text_offset=word_offset+offset,
                shape_class='senna' if base in 'بتثنيئى' and form in ('initial', 'medial') else CLASSES[base],
                status='unresolved', model_region_id=None,
                x=None, y=None, w=None, h=None, area=None, raw_coverage=None))
        owners[offset] = previous
    return result, owners


def expected_slots(subword, characters, owners, word_offset=0):
    from automatic.word_audit import teacher
    canonical = {}
    for codepoint,name in teacher().MARKS.items(): canonical.setdefault(name,chr(codepoint))
    result = []
    for slot in teacher().slots({'characters': characters}):
        offset, name = slot['characterOffset'], slot['type']
        if name == 'small-kaf-body':
            continue  # An auxiliary body contour, not a classified mark.
        if name == 'maddah' and offset and characters[offset]['text'] == 'ٓ' and characters[offset-1]['text'] == 'أ':
            name = 'fatha'  # User's exact أٓ equivalence, shared with mark_inventory.
        owner = None if name.startswith('waqf-') else owners[offset]
        source_character=characters[offset]['text']
        dot_count={'single-dot':1,'double-dot':2,'triple-dot':3}.get(name)
        character=None if dot_count else canonical.get(name) if source_character in BODY_LETTERS else source_character
        if name=='fatha' and source_character=='ٓ': character='َ'
        result.append(dict(id=f"{subword['id']}-slot{len(result)+1}", subword_id=subword['id'],
            letter_id=owner, type=name, side=slot['side'], text_offset=offset, mark_id=None,
            character=character,source_character=source_character,word_text_offset=word_offset+offset,
            dot_count=dot_count))
    return result


def align_bodies(letters, bodies):
    """Count-matched RTL order proposes identities; unknown cuts remain explicit.

    This does not use template cuts or alter model regions to fit the transcript.
    Equal counts are a consistency check, never a segmentation accuracy score.
    """
    import numpy as np
    if len(bodies) != len(letters) or not letters:
        return {}, 'count-mismatch'
    ordered = sorted(bodies, key=lambda r: -float(np.where(r['mask'])[1].mean()))
    centers = [float(np.where(r['mask'])[1].mean()) for r in ordered]
    if any(a-b < 2 for a, b in zip(centers, centers[1:])):
        return {}, 'ambiguous-order'
    return {b['id']: l['id'] for b, l in zip(ordered, letters)}, 'rtl-order'


def assign_marks(marks, slots, anchors):
    """Assign existing subword marks by transcript quotas and body geometry.

    Learned classes, confidences and type labels are deliberately not read.
    Native dot/mark/sign roles describe existing segmentation. Slots of different
    types on the same letter are interchangeable for ownership: an arbitrary
    internal slot permutation must not create a false ambiguity.
    """
    import numpy as np
    from scipy.optimize import linear_sum_assignment
    results = {m['id']: dict(predicted_letter_id=None, status='unresolved',
               method='no-transcript-slot', margin=None, candidates=[]) for m in marks}
    def slot_role(slot):
        if slot['letter_id'] is None: return 'symbol'
        return 'dot' if slot['type'].endswith('-dot') else 'mark'
    for role in ('dot', 'mark', 'symbol'):
        group = sorted((m for m in marks if m['kind'] == role), key=lambda m: m['id'])
        expected = [s for s in slots if slot_role(s) == role]
        owners = {s['letter_id'] for s in expected}
        for mark in group:
            results[mark['id']]['candidates'] = sorted(o for o in owners if o)
        if len(group) != len(expected):
            for mark in group: results[mark['id']]['method'] = 'mark-count-mismatch'
            continue
        if not group: continue
        if role == 'symbol':
            for mark in group:
                results[mark['id']].update(status='word-sign', method='native-word-sign')
            continue
        if len(owners) == 1:
            owner = next(iter(owners))
            for mark in group:
                results[mark['id']].update(predicted_letter_id=owner,
                    status='proposed', method='unique-transcript-owner')
            continue
        if any(owner not in anchors for owner in owners):
            for mark in group: results[mark['id']]['method'] = 'unresolved-body-alignment'
            continue
        costs = np.empty((len(group), len(expected)))
        for a, mark in enumerate(group):
            mx, my = mark['x']+mark['w']/2, mark['y']+mark['h']/2
            for b, slot in enumerate(expected):
                body = anchors[slot['letter_id']]
                distance = abs(mx-(body['x']+body['w']/2))/max(8, body['w'])
                wrong_side = (slot['side'] == 'above' and my > body['y']+body['h'] or
                              slot['side'] == 'below' and my < body['y'])
                costs[a, b] = distance + (2 if wrong_side else 0)
        rr, cc = linear_sum_assignment(costs)
        best = float(costs[rr, cc].sum())
        for r, c in zip(rr, cc):
            owner = expected[c]['letter_id']
            alternative = costs.copy()
            # Test an alternative LETTER, not a permutation of marks on it.
            alternative[r, [i for i,s in enumerate(expected) if s['letter_id']==owner]] = 1e6
            ar, ac = linear_sum_assignment(alternative)
            margin = float(alternative[ar, ac].sum()-best)
            target = results[group[r]['id']]
            target.update(margin=margin, method='transcript-quota-and-geometry')
            if margin < .15 or costs[r, c] > 3:
                target['method'] = 'ambiguous-geometry'
                continue
            target.update(predicted_letter_id=owner, status='proposed')
    return results
