"""Coverage selection and source-raster invariants for template repeats."""
import importlib.util
import unittest

AVAILABLE=all(importlib.util.find_spec(x) for x in ['numpy','PIL','PySide6'])
if AVAILABLE:
    from letter_template_repeat import cases, choose_diverse, complete_row, cover_used_templates, raster_runs


@unittest.skipUnless(AVAILABLE,'Optional raster dependencies are absent')
class TemplateRepeatTests(unittest.TestCase):
    def test_template_coverage_moves_whole_validation_pages_and_reserves_test_and_reviewed_pages(self):
        def item(identifier, page, template, bases=2):
            return dict(id=identifier, page=page, baseCharacters=bases, cases=[template + '|used|*'])
        pools = dict(train=[item('train', 1, 'common')],
                     validation=[item('rare', 2, 'rare'), item('same-page', 2, 'common'),
                                 item('single', 2, 'common', 1), item('reviewed', 3, 'protected')],
                     test=[item('test', 4, 'test-only')])
        pages = {1:dict(split='train'), 2:dict(split='validation'),
                 3:dict(split='validation'), 4:dict(split='test')}
        overrides = cover_used_templates(pools, pages, {'common','rare','protected','test-only'}, {3})
        self.assertEqual(set(overrides), {'page002'})
        self.assertEqual({row['id'] for row in pools['train']}, {'train','rare','same-page'})
        self.assertEqual([row['id'] for row in pools['validation']], ['reviewed'])
        self.assertEqual([row['id'] for row in pools['test']], ['test'])
        self.assertEqual(pages[3]['split'], 'validation')
        self.assertEqual(pages[4]['split'], 'test')

    def test_coverage_includes_boundary_refinements_and_assembled_rules(self):
        row = dict(letters=[dict(character='ه', position='medial', template='heh-body',
                                boundaryTemplate='heh-lip', entryTemplate='shared-entry')],
                   templates=['heh-body', 'attachment'])
        self.assertEqual(set(cases(row)), {
            'heh-body|medial|ه', 'heh-lip|medial|ه', 'shared-entry|medial|ه',
            'heh-body|used|*', 'attachment|used|*'})

    def test_automatic_targets_require_all_letters_to_match(self):
        row = dict(complete=True, sourceInk={'commands': ['ink']},
                   bodies=[{}, {}], letters=[dict(status='matched'), dict(status='matched')])
        self.assertTrue(complete_row(row, set(), 'example'))
        self.assertFalse(complete_row(row, {'example'}, 'example'))
        row['letters'][0]['status'] = 'no-match'
        self.assertFalse(complete_row(row, set(), 'example'))

    def test_rare_case_survives_selection(self):
        pool=[dict(id=str(i),cases=['common'],signature=str(i)) for i in range(100)]
        pool.append(dict(id='rare',cases=['rare','common'],signature='rare'))
        selected=choose_diverse(pool,2,10,63)
        self.assertEqual(len(selected),10)
        self.assertIn('rare',{r['id'] for r in selected})
        self.assertEqual(selected,choose_diverse(pool,2,10,63))

    def test_selection_refuses_to_hide_uncovered_cases(self):
        pool=[dict(id=str(i),cases=[str(i)],signature=str(i)) for i in range(3)]
        with self.assertRaisesRegex(ValueError,'Coverage needs'):
            choose_diverse(pool,1,2,63)

    def test_qt_raster_preserves_counter_and_page_coordinates(self):
        data=dict(fillRule='evenodd',commands=[['M',2,2],['L',12,2],['L',12,12],['L',2,12],['L',2,2],
            ['M',5,5],['L',9,5],['L',9,9],['L',5,9],['L',5,5]])
        runs=raster_runs(data,40,40,[2,0,0,2,0,0])
        pixels={p for start,n in zip(runs[::2],runs[1::2]) for p in range(start,start+n)}
        self.assertIn(5*40+5,pixels)
        self.assertNotIn(12*40+12,pixels)
        self.assertTrue(all(0<=p<1600 for p in pixels))
        self.assertEqual(len(pixels),400-64)


if __name__=='__main__':unittest.main()
