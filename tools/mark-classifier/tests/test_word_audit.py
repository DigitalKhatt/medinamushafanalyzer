import json
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from automatic.word_audit import (check_subword, expected_inventory, inventory_check,
    alignment, letter_status, mask_union, ownership_hints, page_geometry)


def item(id, type, word='w1', sub='s1', role='mark'):
    return dict(id=id, final_kind=role, final_type=type, prediction_kind=role,
                prediction_type=type, teacher_kind=role, teacher_type=type,
                human_reviewed=0, model_confidence=.99, prediction_guard=None,
                word_id=word, subword_id=sub)


class WordAuditTests(unittest.TestCase):
    def test_missing_native_alignment_never_falls_back_to_cached_expected_marks(self):
        sub=dict(id='s',text='بَ',expected=json.dumps([dict(text='ب',position='isolated'),dict(text='َ',position='unknown')]))
        check=check_subword(sub,[item('fatha','fatha')],characters=None)
        self.assertIsNone(check['inventories']['final']['expected'])
        self.assertIsNone(check['inventories']['final']['missing'])
        self.assertFalse(check['inventories']['final']['checked'])

    def test_edition_dots_hamza_and_small_kaf_conventions(self):
        chars=[{'text':c,'position':p} for c,p in [('ي','final'),('ي','initial'),
               ('ش','medial'),('إ','isolated'),('ِ','unknown'),('ك','isolated')]]
        self.assertEqual(expected_inventory(chars), {'double-dot':1,'triple-dot':1,'hamza-below':1,'kasra':1})
        self.assertEqual(expected_inventory([{'text':'۞','position':'unknown'}]), {'hizb-sign':1})
        self.assertEqual(expected_inventory([{'text':'۩','position':'unknown'}]), {'quranic-sign':1})

    def test_alignment_checks_tiny_marks_individually_and_unions_overlapping_paths(self):
        body=dict(id='body',runs=[0,100],area=100)
        dot=dict(id='dot',runs=[200,1],area=1)
        result=alignment([body,dot],[[0,100],[0,20],[201,1]])
        self.assertEqual(result['mismatches'],[dict(id='dot',sourceCoverage=0.)])
        self.assertEqual(alignment([body,dot],[[0,100],[200,1]])['mismatches'],[])

    def test_equal_word_totals_cannot_hide_swapped_subword_marks(self):
        a={'id':'s1','text':'بَ','expected':json.dumps([{'text':'ب','position':'isolated'},{'text':'َ','position':'unknown'}])}
        b={'id':'s2','text':'رِ','expected':json.dumps([{'text':'ر','position':'isolated'},{'text':'ِ','position':'unknown'}])}
        one=[item('dot','single-dot',role='dot'),item('kasra','kasra')]
        two=[item('fatha','fatha',sub='s2')]
        expected=expected_inventory(json.loads(a['expected']))+expected_inventory(json.loads(b['expected']))
        self.assertTrue(inventory_check(expected,one+two)['matches'])
        self.assertEqual(check_subword(a,one)['inventories']['final']['missing'],{'fatha':1})
        self.assertEqual(check_subword(b,two)['inventories']['final']['extra'],{'fatha':1})
        self.assertIn('missing-body',check_subword(a,one)['issues'])

    def test_extra_mark_transfer_keeps_all_identical_occurrences_ambiguous(self):
        def word(id,n,missing,extra):
            return dict(id=id,line=1,ordinal=n,inventories={'final':dict(missing=missing,extra=extra)})
        records=[word('w1',1,{'kasra':1},{}),word('w2',2,{}, {'kasra':1}),word('w3',3,{}, {})]
        items=[item('a','kasra','w2'),item('b','kasra','w2')]
        hints=ownership_hints(records,items)
        self.assertEqual(len(hints),1)
        self.assertEqual(hints[0]['candidates'][0]['itemIds'],['a','b'])
        self.assertEqual(hints[0]['status'],'ownership-proposal')
        self.assertEqual(items[0]['word_id'],'w2')
        records[1]['line']=2
        self.assertEqual(ownership_hints(records,items),[])

    def test_tiny_ink_conservation_detects_overlap_and_equal_area_displacement(self):
        rows=[dict(runs=[0,1,4,2],area=3)]
        self.assertEqual(sum(mask_union(rows,10)),3)
        with self.assertRaisesRegex(ValueError,'Overlapping'):
            mask_union(rows+[dict(runs=[4,1],area=1)],10)
        with self.assertRaisesRegex(ValueError,'differs'):
            page_geometry(dict(width=5,height=2,ink_pixels=3),rows,
                          {'items':[dict(runs=[0,1,5,2],area=3)]})
        for runs in ([1],[-1,1],[0,0],[9,2],[2,3,3,1]):
            with self.assertRaises(ValueError):mask_union([dict(runs=runs,area=1)],10)

    def test_untrained_unknown_and_role_type_disagreement_remain_visible(self):
        i=item('a','fatha',role='subword-ink')
        i.update(teacher_kind='mark',teacher_type='fatha')
        self.assertEqual(inventory_check({'fatha':1},[i])['missing'],{'fatha':1})
        self.assertTrue(inventory_check({'fatha':1},[i],'proposal')['matches'])

    def test_letter_cache_is_never_current_after_template_or_version_changes(self):
        cache=dict(templateConfig={},version=54,pageDataSha256='page',fontSha256='font',subwords=[])
        prov=dict(cacheSha256='page',templateSha256='font')
        word=dict(line=1,ordinal=1)
        self.assertEqual(letter_status(None,word,[],{},54,prov)['status'],'not-computed')
        self.assertEqual(letter_status(cache,word,[],{},55,prov)['status'],'stale')
        self.assertEqual(letter_status(cache,word,[],{'changed':True},54,prov)['status'],'stale')


if __name__ == '__main__':unittest.main()
