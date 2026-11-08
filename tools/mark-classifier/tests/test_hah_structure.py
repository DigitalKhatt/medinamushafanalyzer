import copy
import inspect
import json
from pathlib import Path
import unittest
from workbench_paths import NATIVE

try:
    import numpy as np
    from PySide6.QtCore import QPointF
    import hah_structure as shape
except ImportError:
    shape=None

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipIf(shape is None,'Optional structural experiment dependencies unavailable')
class HahStructureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config=json.loads((NATIVE/'templates/experimental/hah-structure.json').read_text())
        cls.examples=json.loads((ROOT/'tests/fixtures/hah_structure_ink.json').read_text())['words']
        cls.results=[shape.detect(w['ink'],cls.config) for w in cls.examples]

    def test_stacked_and_flat_heads_generate_their_own_cuts(self):
        for result in self.results:
            self.assertGreater(len(result['structures']),0)
            self.assertGreater(len(result['probes']),0)
            for structure in result['structures']:
                self.assertGreater(structure['stroke'],0)
                self.assertGreaterEqual(len(structure['mouthSamples']),2)
                self.assertIn('attachment',structure)

    def test_opening_is_actual_white_space_and_required_ink_is_inside_source(self):
        for word,result in zip(self.examples,self.results):
            path=shape.painter_path(word['ink'])
            for structure in result['structures']:
                for sample in structure['mouthSamples']:
                    self.assertFalse(path.contains(QPointF(*sample['air'])))
            for proposal in result['probes']:
                for point in proposal['requiredInk']:self.assertTrue(path.contains(QPointF(*point)))

    def test_uniform_scale_and_translation_preserve_structural_locations(self):
        ink=copy.deepcopy(self.examples[0]['ink']);scale=1.6;offset=np.array([37.,-21.])
        for command in ink['commands']:
            points=np.array(command[1:]).reshape(-1,2)*scale+offset;command[1:]=points.ravel().tolist()
        transformed=shape.detect(ink,self.config)
        self.assertEqual(len(transformed['structures']),len(self.results[0]['structures']))
        for a,b in zip(self.results[0]['structures'],transformed['structures']):
            for feature in ['root','lipTip','returnOutlet','attachment']:
                np.testing.assert_allclose(np.array(a[feature])*scale+offset,b[feature],atol=.03)

    def test_no_routes_or_transcript_used_and_input_is_not_modified(self):
        ink=copy.deepcopy(self.examples[1]['ink']);original=copy.deepcopy(ink)
        ink['oldCandidates']=[{'right':[999,999],'left':[-999,-999]}];ink['transcript']='wrong text'
        result=shape.detect(ink,self.config)
        self.assertEqual(result,self.results[1])
        self.assertEqual(ink['commands'],original['commands'])
        source=inspect.getsource(shape)
        for banned in ['searchContourWalk(', 'contourBodyBoundaries(', 'letter-forms.json']:
            self.assertNotIn(banned,source)

    def test_closed_counter_and_vertical_stem_are_not_open_hah(self):
        shapes=[{'fillRule':'nonzero','commands':[['M',0,0],['L',1,0],['L',1,10],['L',0,10],['L',0,0]]},
                {'fillRule':'evenodd','commands':[['M',0,0],['L',12,0],['L',12,6],['L',0,6],['L',0,0],
                                                  ['M',2,2],['L',10,2],['L',10,4],['L',2,4],['L',2,2]]}]
        for ink in shapes:self.assertEqual(shape.detect(ink,self.config)['probes'],[])

    def test_each_normal_has_a_nonzero_direction_and_two_distinct_endpoints(self):
        for result in self.results:
            for candidate in result['probes']:
                for name in ['rightProbe','leftProbe']:
                    p=candidate[name];self.assertAlmostEqual(np.linalg.norm(p['normal']),1.,places=8)
                    self.assertGreater(np.linalg.norm(np.subtract(*p['line'])),p['stroke'])

    def test_flat_head_attachment_is_beyond_mouth_root(self):
        # The mouth root forks into lip/return/head-extension. The attachment
        # is at the next junction beside the terminal cap, not through the head.
        flat=next(r for w,r in zip(self.examples,self.results) if w['id']=='page003:l15-w3-s2')
        refined=[f for f in flat['structures'] if f['attachmentRefined']]
        self.assertTrue(refined)
        self.assertTrue(all(f['attachment'][0]>f['root'][0] for f in refined))
        self.assertTrue(all(f['headCap'] for f in refined))

    def test_attaching_stroke_stays_with_flat_hah_until_its_bend(self):
        flat=next(r for w,r in zip(self.examples,self.results) if w['id']=='page003:l15-w3-s2')
        shapes=[f for f in flat['structures'] if f['entryRule']=='attachment-turn']
        self.assertTrue(shapes)
        for f in shapes:
            self.assertGreater(f['attachmentStrokeLength'],1.)
            self.assertGreater(f['attachmentStrokeEnd'][1],f['attachment'][1])
            proposals=[p for p in flat['probes'] if p['featureId']==f['id']]
            self.assertTrue(proposals)
            for p in proposals:
                self.assertEqual(p['parameters']['entryReference'],'attachment-turn')
                self.assertGreaterEqual(p['parameters']['entryOffset'],0)
                # Body ownership is enforced by the native partition coverage
                # check, including points down the newly owned attachment.
                self.assertTrue(any(q[1]>f['attachment'][1]+f['stroke'] for q in p['requiredInk']))

    def test_attachment_stops_at_its_first_bend_before_the_next_ascender(self):
        theta=np.linspace(np.pi,np.pi/2,60)
        curve=np.column_stack((1+np.cos(theta),2+np.sin(theta)))
        points=np.vstack(([[0,0],[0,1]],curve,[[3,3],[5,3],[5,0],[5,-8]]))
        settings=self.config['entry']['attachmentStroke']
        end=shape.attachment_stroke_end(points,1.,settings,.4)
        self.assertIsNotNone(end)
        cut=shape.at_distance(points,end)
        self.assertGreater(cut[0],.5)
        self.assertLess(cut[0],1.2)
        self.assertGreater(cut[1],2.8)
        shifted=points*2.3+np.array([30,-9])
        other=shape.attachment_stroke_end(shifted,2.3,settings,.4)
        self.assertAlmostEqual(other,end*2.3,places=8)

    def test_attachment_does_not_consume_a_straight_join_or_distant_bend(self):
        settings=self.config['entry']['attachmentStroke']
        for points in [[[0,0],[8,0]],[[0,0],[0,10],[5,10]]]:
            self.assertIsNone(shape.attachment_stroke_end(np.array(points,float),1.,settings,.4))

if __name__=='__main__':unittest.main()
