from contextlib import closing
import io
import json
from pathlib import Path
import sqlite3
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from comparison import connected_summary
from data import add_diverse_focus, connected_rows, mine_training
from letter_retest import mask_decode, seen_target_classes
from seen import families, layout, measure


def letter(index, character, position, template):
    return dict(letterIndex=index, character=character, position=position, template=template)


class ConnectedWorkflowTest(unittest.TestCase):
    def test_shared_seen_class_preserves_instance_and_export_roles(self):
        letters = [letter(1, 'س', 'initial', 'seen-init'), letter(2, 'ب', 'medial', 'beh-tooth-medi'),
                   letter(3, 'ش', 'final', 'seen-fina')]
        reference_roles = np.array([0, 1, 1, 1, 2])
        np.testing.assert_array_equal(seen_target_classes(letters, reference_roles), [0, 3, 1, 3, 2])
        np.testing.assert_array_equal(reference_roles, [0, 1, 1, 1, 2])

        masks = np.eye(4, dtype=np.float32).reshape(4, 1, 4)
        scores = np.array([[.02, .02, .9, .06], [.9, .02, .02, .06],
                           [.02, .9, .02, .06], [.02, .02, .02, .94]])
        pred, roles, _ = mask_decode(masks, scores, np.array([[True, True, True, False]]),
                                     complete=False, min_region_area=1)
        np.testing.assert_array_equal(pred, [[1, 2, 3, 0]])
        np.testing.assert_array_equal(roles, [0, 1, 1, 2])

    def test_source_scope_ignores_marks_tatweel_and_single_base_letters(self):
        rows = [dict(line=1, word=1, text='سَـٔ'), dict(line=1, word=2, text='سَـهْ'),
                dict(line=1, word=3, text='بسم'), dict(line=2, word=1, text='بسم')]
        cache = dict(subwords=rows, sourceLines=[dict(line=1, transcriptWords=2), dict(line=2, transcriptWords=0)],
                     sourceWords=[dict(line=1, word=1, inTranscript=True), dict(line=1, word=2, inTranscript=True),
                                  dict(line=1, word=3, inTranscript=False), dict(line=2, word=1, inTranscript=True)])
        self.assertEqual(connected_rows(cache), [rows[1]])

    def test_fragmentation_and_genuine_three_teeth_are_distinct(self):
        seen = [letter(1, 'ش', 'initial', 'seen-init')]
        truth = np.ones((3, 10), 'uint16')
        pred = np.repeat(np.arange(1, 4, dtype='uint16')[:, None], 10, axis=1)
        roles = np.array([0, 1, 1, 1])
        split = measure(seen, truth, pred, roles)
        self.assertEqual(split['fragments'], 3)
        self.assertEqual(split['seen'][0]['strayBodies'], 2)
        tooth = [letter(i, c, 'medial', 'beh-tooth-medi') for i, c in enumerate('بين', 1)]
        self.assertEqual(measure(tooth, pred, pred, roles)['triples'], [dict(separate=True, merged=False)])
        self.assertEqual(measure(tooth, pred, truth, roles[:2])['triples'], [dict(separate=False, merged=True)])

    def test_extra_small_body_is_visible_below_substantial_fragment_threshold(self):
        truth = np.ones((10, 10), 'uint16')
        pred = truth.copy()
        pred[0, :4] = 2
        stats = measure([letter(1, 'س', 'initial', 'seen-init')], truth, pred, np.array([0, 1, 1]))
        self.assertEqual(stats['fragments'], 1)
        self.assertEqual(stats['seen'][0]['strayBodies'], 1)
        self.assertAlmostEqual(stats['seen'][0]['iou'], .96)

    def test_focused_sampler_covers_the_new_shape_families_and_tooth_controls(self):
        initial = [letter(1, 'س', 'initial', 'seen-init'), letter(2, 'ن', 'final', 'noon-fina')]
        medial = [letter(1, 'ب', 'initial', 'beh-init'), letter(2, 'ش', 'medial', 'seen-medi'),
                  letter(3, 'ه', 'final', 'heh-counter-fina')]
        pool = [dict(id='a', families=families(initial, layout(initial)[0]), triple=False, signature='سن'),
                dict(id='b', families=families(medial, layout(medial)[0]), triple=False, signature='بشه'),
                dict(id='c', families=[], triple=True, signature='بيني')]
        focus = {}
        add_diverse_focus(pool, focus, per_family=1, tooth_controls=1)
        self.assertEqual(focus, {'a': 'seen-initial-س', 'b': 'seen-medial-before-heh-ش',
                                 'c': 'separate-teeth-control'})

    def test_hard_example_mining_checks_train_split_and_current_mask(self):
        page = 'page003'
        letters = [letter(1, 'س', 'initial', 'seen-init'), letter(2, 'ن', 'final', 'noon-fina')]
        reference = np.repeat([[1, 1, 1, 2]], 9, axis=0).astype('uint16')
        prediction = np.repeat(np.arange(1, 5, dtype='uint16')[None, :], 9, axis=0)
        prediction[:, :3] = np.repeat(np.arange(1, 4, dtype='uint16'), 3)[:, None]
        roles = np.array([0, 1, 1])
        blob = io.BytesIO()
        np.savez(blob, ink=reference > 0, reference=reference, roles=roles,
                 prediction=prediction, predicted_roles=np.array([0, 1, 1, 1, 1]))
        rows = [dict(line=1, word=i, subword=1) for i in range(1, 4)]
        pool = [dict(id=f'{page}:l1-w{i}-s1', page=page, families=['initial-س'], triple=False,
                     signature=i, letters=letters) for i in range(1, 4)]

        def current(row, _):
            return dict(status={'valid': True}, ink=reference > 0, roles=roles,
                        reference=reference if row['word'] == 1 else np.fliplr(reference))

        with TemporaryDirectory() as directory:
            audit = Path(directory) / 'audit.sqlite'
            with closing(sqlite3.connect(audit)) as db, db:
                db.execute('CREATE TABLE examples(id,split,reference,detail,count_error,masks)')
                db.executemany('INSERT INTO examples VALUES (?,?,?,?,?,?)', [
                    (item['id'], split, 'complete', '{"expectedBodies":2}', 2, blob.getvalue())
                    for item, split in zip(pool, ('train', 'train', 'validation'), strict=True)
                ])
            focus = {}
            with (patch('data.load_page'), patch('data.connected_rows', return_value=rows),
                  patch('data.document'), patch('data.prepare_example', side_effect=current)):
                counts = mine_training(audit, pool, Path(directory), {}, {page: {}}, focus)
        self.assertEqual(focus, {pool[0]['id']: 'seen-fragmented'})
        self.assertEqual(counts, {'seen-fragmented': 1})

    def test_archive_summary_excludes_singles(self):
        with TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'summary.json').write_text(json.dumps(dict(examples=2, pages=1, elapsedSeconds=10)))
            with closing(sqlite3.connect(':memory:')) as db:
                db.execute('CREATE TABLE examples(id,detail,count_error,training,split,reference)')
                db.executemany('INSERT INTO examples VALUES (?,?,?,?,?,?)', [
                    ('page003:l1-w1-s1', '{"expectedBodies":1}', 1, 0, 'validation', 'complete'),
                    ('page003:l1-w2-s1', '{"expectedBodies":2}', 0, 1, 'train', 'complete')])
                summary = connected_summary(folder, db)
            self.assertEqual(summary['examples'], 1)
            self.assertEqual(summary['singleLetterPassthrough'], 1)
            self.assertEqual(summary['bodyCountDifferences'], 0)
            self.assertTrue(summary['filteredArchive'])


if __name__ == '__main__':
    unittest.main()
