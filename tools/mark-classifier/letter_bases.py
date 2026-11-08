"""Base characters that own letter bodies in the Hafs corpus."""

BODY_LETTERS = frozenset('ءاأإآٱبتثجحخدذرزسشصضطظعغفقكلمنهويىؤئة')
MIN_TRAINING_BASES = 2


def base_character_count(text):
    return sum(char in BODY_LETTERS for char in text)


def has_training_boundary(record):
    """Use character identities or reviewed owners, independently of mask fragments."""
    counts = []
    text = record.get('textForDisplayOnly')
    if text is not None:
        counts.append(base_character_count(text))
    elif 'letters' in record:
        counts.append(sum(letter['character'] in BODY_LETTERS for letter in record['letters']))
    if 'baseCharacterCount' in record:
        counts.append(record['baseCharacterCount'])
    if not counts:
        return False
    if 'bodyRegions' in record:
        counts.append(record['bodyRegions'])
    return min(counts) >= MIN_TRAINING_BASES


def check_training_records(records):
    for record in records:
        if record['group'] in ('weak_train', 'human_train') and not has_training_boundary(record):
            raise ValueError(f"{record['id']} needs two confirmed base characters. Prepare a multi-character training set before fitting.")
