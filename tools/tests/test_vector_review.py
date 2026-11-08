import sys
import unittest
import json
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from vector_review import prepare_vector_review as v

class VectorReviewTests(unittest.TestCase):
    def dot_fixture(self):
        return json.loads((Path(__file__).parent/'fixtures/dot-pair-regressions.json').read_text())
    def test_distorted_native_pair_uses_train_shape_evidence_not_transcript_counts(self):
        f=self.dot_fixture();path=f['page228']['paths'][0]
        self.assertIsNone(v.strong_type(path))
        evidence=v.double_dot_evidence(path,f['bank'],100)
        self.assertLess(evidence['distance'],.12)
        self.assertEqual(v.strong_type({**path,'dotShapeEvidence':evidence}),'double-dot')
        # A competing prototype with the exact same shape makes this ambiguous.
        bank={**f['bank'],'fatha':[v.features(path)]}
        self.assertIsNone(v.double_dot_evidence(path,bank,100))
    def test_packed_pairs_split_then_form_one_double_and_one_triple(self):
        from vector_review.stroke_geometry import connected_ink
        f=self.dot_fixture();packed,single=f['page184']['paths']
        parts=v.split_dot_pairs(packed,100,f['bank']);self.assertEqual(len(parts),2)
        original={p for c in connected_ink(packed['runs'],100) for p in c}
        result=[p for part in parts for c in connected_ink(part['runs'],100) for p in c]
        self.assertEqual(len(result),len(set(result)));self.assertEqual(set(result),original)
        assigned,reason=v.assignment([*parts,single],[{'type':'double-dot','capacity':1,'side':'above'},
            {'type':'triple-dot','capacity':2,'side':'above'}],f['bank'],baseline=60,stroke_grouping=True)
        self.assertIsNone(reason)
        triple=next(indices for slot,indices,_ in assigned if slot['type']=='triple-dot')
        self.assertEqual(triple,(1,2))
    def test_packed_pair_split_rejects_unknown_shape_and_native_contour_mismatch(self):
        f=self.dot_fixture();path=f['page184']['paths'][0]
        self.assertEqual(v.split_dot_pairs(path,100,{}),[path])
        changed={**path,'contourCount':3}
        self.assertEqual(v.split_dot_pairs(changed,100,f['bank']),[changed])
    def test_dot_fallback_rejects_counters(self):
        from vector_review.dot_geometry import has_counter
        ring={(y,x) for y in range(5) for x in range(8) if y in (0,4) or x in (0,7)}
        self.assertTrue(has_counter(ring));self.assertFalse(has_counter(ring-{(0,3)}))
    def test_packed_split_cannot_push_assignment_past_its_path_budget(self):
        f=self.dot_fixture();packed=f['page184']['paths'][0]
        paths=[self.path('body',0),packed]+[self.path(f'one-{i}',i*10) for i in range(13)]
        sub={'id':'s','text':'ت','characters':[{'text':'ت','position':'initial'}],
             'pathIds':[p['id'] for p in paths]}
        word={'id':'w','line':1,'text':'ت','legacyFlags':1,'transcriptMatches':True,'subwords':[sub]}
        result=v.prepare({'paths':paths,'words':[word],'raster':{'width':100}},f['bank'],stroke_grouping=True)
        self.assertEqual(len(result['paths']),len(paths))
        self.assertTrue(any(c['pathIds']==[packed['id']] for c in result['candidates']))
    def test_contextual_yeh_dots_and_final_kaf_are_not_generic_marks(self):
        sub={'characters':[{'text':'ي','position':'initial'},{'text':'ك','position':'final'}]}
        self.assertEqual([s['type'] for s in v.slots(sub)],['double-dot','small-kaf-body'])
        sub['characters'][0]['position']='final'
        self.assertEqual([s['type'] for s in v.slots(sub)],['small-kaf-body'])
    def test_precomposed_hamza_and_quranic_marks(self):
        sub={'characters':[{'text':c,'position':'unknown'} for c in 'إِ\u06e1']}
        self.assertEqual([s['type'] for s in v.slots(sub)],['hamza-below','kasra','sukun'])
    def path(self,name,x):
        return {'id':name,'bbox':[x,0,5,5],'contourCount':1,'signature':'f'*64,'templateScores':{'onedot':0.01}}
    def cluster(self):
        a,b=self.path('one',2),self.path('pair',0);b['bbox']=[0,6,10,6];b['templateScores']={'twodots':.01};return a,b
    def test_triple_dot_has_one_semantic_instance_with_two_ink_paths(self):
        a,b=self.cluster()
        assigned,reason=v.assignment([a,b],[{'type':'triple-dot','capacity':2}],{})
        self.assertIsNone(reason);self.assertEqual(assigned[0][1],(0,1))
    def test_dp_abstains_instead_of_using_distant_dot_cluster(self):
        a,b=self.cluster();b['bbox'][0]=200
        assigned,reason=v.assignment([a,b],[{'type':'triple-dot','capacity':2}],{})
        self.assertEqual(assigned,[]);self.assertIn('unmatched',reason)
    def test_two_single_dots_are_not_a_triple_dot(self):
        assigned,reason=v.assignment([self.path('one',0),self.path('two',7)],[{'type':'triple-dot','capacity':2}],{})
        self.assertEqual(assigned,[]);self.assertIn('unmatched',reason)
    def test_dot_cluster_distance_is_relative_to_dot_size(self):
        a,b=self.cluster();b['bbox'][0]=40
        assigned,reason=v.assignment([a,b],[{'type':'triple-dot','capacity':2}],{})
        self.assertEqual(assigned,[]);self.assertIn('unmatched',reason)
    def test_above_triple_dot_cannot_use_below_letter_dots(self):
        a,b=self.cluster()
        slot={'type':'triple-dot','capacity':2,'side':'above'}
        assigned,reason=v.assignment([a,b],[slot],{},baseline=0)
        self.assertEqual(assigned,[])
        assigned,reason=v.assignment([a,b],[slot],{},baseline=10)
        self.assertTrue(assigned);self.assertIsNone(reason)
    def test_expected_dot_side_is_distinct_from_dot_shape(self):
        sub={'characters':[{'text':c,'position':'initial'} for c in 'بنتي']}
        self.assertEqual([s['side'] for s in v.slots(sub)],['below','above','above','below'])
    def test_triple_dot_prefers_the_nearest_compatible_pair(self):
        a,c=self.cluster();b=self.path('far-pair',20);b['bbox']=[20,6,10,6];b['templateScores']={'twodots':.01}
        assigned,_=v.assignment([a,b,c],[{'type':'triple-dot','capacity':2},{'type':'double-dot','capacity':1}],{})
        triple=next(indices for slot,indices,_ in assigned if slot['type']=='triple-dot')
        self.assertEqual(triple,(0,2))
    def test_triple_dot_rejects_neighboring_letters_and_accepts_deformed_lower_pair(self):
        cases=[([1061.24,866.50,9.51,9.45],[1080.65,857.69,18.19,10.92],[1060.2,875.6,18.3,12.2]),
               ([337.2,1859.7,9.5,9.4],[409.7,1859.4,18.2,10.9],[335.6,1869.7,18.5,12.0]),
               ([674.2,714.2,9.5,9.5],[701.1,739.9,18.4,11.7],[673.2,723.2,18.3,12.2])]
        for single,neighbor,lower in cases:
            a,b,c=self.path('single',0),self.path('neighbor',0),self.path('correct-lower',0)
            a['bbox']=single;b['bbox']=neighbor;c['bbox']=lower
            b['templateScores']={'twodots':.03};c['templateScores']={'twodots':.24}
            selected,reason=v.assignment([a,b,c],[{'type':'triple-dot','capacity':2}],{})
            self.assertEqual(selected[0][1],(0,2));self.assertIsNone(reason)
    def test_deformed_lower_pair_is_grouped_only_in_a_complete_triple_layout(self):
        # Train-page example: the lower pair is less like the standalone
        # template, but its complete single-above-pair layout is unambiguous.
        a,b=self.cluster()
        a['bbox']=[360.4772,2149.7604,9.4173,9.4473]
        b['bbox']=[359.6665,2160.2995,19.6016,11.6649]
        b['templateScores']={'twodots':.3100504671}
        slot={'type':'triple-dot','capacity':2,'side':'above'}
        selected,reason=v.assignment([a,b],[slot],{},baseline=2192.36)
        self.assertIsNone(reason);self.assertEqual(selected[0][1],(0,1))
        self.assertIsNone(v.strong_type(b))  # Standalone threshold is unchanged.
        for change in ({'bbox':[410,2160.3,19.6,11.7]},
                       {'bbox':[359.7,2130,19.6,11.7]},
                       {'templateScores':{'twodots':.5}}):
            selected,_=v.assignment([a,{**b,**change}],[slot],{},baseline=2192.36)
            self.assertEqual(selected,[])

    def test_digits_and_detached_small_letters_are_not_subword_bodies(self):
        def sub(id,text,path):
            return {'id':id,'text':text,'pathIds':[path],'characters':[{'text':text,'position':'unknown'}]}
        paths=[self.path('body',0),self.path('small',10),self.path('digit-a',20),self.path('digit-b',30)]
        for path in paths:path.update(templateScores={},wordId='w1')
        word={'id':'w1','line':1,'text':'هُۥ','legacyFlags':1,'transcriptMatches':True,
            'subwords':[sub('s1','ه','body'),sub('s2','ۥ','small')]}
        number={'id':'w2','line':1,'text':'١٠','legacyFlags':1,'transcriptMatches':True,
            'subwords':[sub('s3','١','digit-a'),sub('s4','٠','digit-b')]}
        result=v.prepare({'paths':paths,'words':[word,number]}, {})
        self.assertEqual(len(result['subwords']),1)
        self.assertEqual(result['subwords'][0]['text'],'هۥ')
        small=next(c for c in result['candidates'] if c['markType']=='small-waw')
        self.assertEqual(small['kind'],'mark');self.assertEqual(small['subwordId'],'s1')
        verse=next(c for c in result['candidates'] if c['markType']=='verse-number')
        self.assertEqual(verse['pathIds'],['digit-a','digit-b']);self.assertEqual(verse['kind'],'symbol')
    def test_split_preserves_exact_ink_and_does_not_fill_holes(self):
        from vector_review.stroke_geometry import connected_ink, stroke_paths
        runs = []
        for y in (2, 3, 8, 9):
            runs.extend((y * 100 + 10, 20))
        path = {**self.path('two-strokes', 0), 'runs': runs, 'contourCount': 2}
        parts = stroke_paths(path, 100, split=True)
        self.assertEqual(len(parts), 2)
        self.assertEqual([point for part in parts for point in connected_ink(part['runs'], 100)[0]],
                         [point for piece in connected_ink(runs, 100) for point in piece])
        self.assertEqual(stroke_paths(path, 100, split=False), [path])
        ring = [101, 3, 201, 1, 203, 1, 301, 3]
        self.assertEqual(len(connected_ink(ring, 100)), 1)
        self.assertEqual(len(connected_ink(ring, 100)[0]), 8)

    def test_tanwin_requires_two_compatible_strokes(self):
        from vector_review.stroke_geometry import stroke_paths
        paths = []
        for name, x, y in [('upper', 10, 4), ('lower', 11, 10), ('compact', 11, 10), ('staggered', 26, 10), ('offset', 32, 10)]:
            runs = [y * 100 + x, 20, (y + 1) * 100 + x, 20]
            if name == 'compact':
                runs = [value for row in range(y, y + 5) for value in (row * 100 + x, 5)]
            path = {**self.path(name, x), 'runs': runs, 'bbox': [x, y, 20, 2], 'templateScores': {}}
            paths.extend(stroke_paths(path, 100, split=True))
        upper, lower, compact, staggered, offset = paths
        expected = [{'type': 'tanwin-kasr', 'capacity': 2}]
        assigned, _ = v.assignment([upper, lower], expected, {}, stroke_grouping=True)
        self.assertEqual(assigned[0][1], (0, 1))
        assigned, _ = v.assignment([upper, staggered], expected, {}, stroke_grouping=True)
        self.assertEqual(assigned[0][1], (0, 1))
        for other in (compact, offset):
            assigned, reason = v.assignment([upper, other], expected, {}, stroke_grouping=True)
            self.assertEqual(assigned, [])
            self.assertIn('unmatched', reason)

    def test_page_split_matches_workbench_policy(self):
        import hashlib
        for n in range(1,605):
            page=f'page{n:03d}';bucket=int(hashlib.sha256(f'hafs1441-v1:{page}'.encode()).hexdigest()[:8],16)%100
            self.assertEqual(v.split_for(page),'test' if bucket<10 else 'validation' if bucket<20 else 'train')
    def test_validation_and_test_pages_cannot_seed_weak_templates(self):
        page=next(f'page{n:03d}' for n in range(1,605) if v.split_for(f'page{n:03d}')!='train')
        doc={'pageId':page,'paths':[],'words':[{'unreadable':'would fail if used'}]}
        self.assertEqual(dict(v.seed_bank([doc])),{})

if __name__=='__main__':unittest.main()
