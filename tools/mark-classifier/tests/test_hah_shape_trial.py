"""Contracts for the isolated shape experiment, including geometry and page separation."""
import base64
import io
import json
from pathlib import Path
import tempfile
import unittest

try:
    import numpy as np
    from PIL import Image, ImageDraw
    import hah_shape_trial as trial
except ImportError:
    trial = None

@unittest.skipIf(trial is None, 'Optional automatic-experiment dependencies unavailable')
class HahShapeTrialTests(unittest.TestCase):
    def encoded(self, mask):
        stream=io.BytesIO();Image.fromarray(mask.astype('uint8')*255).save(stream,format='PNG')
        return base64.b64encode(stream.getvalue()).decode()

    def body(self, reflected=False):
        image=Image.new('1',(128,128));draw=ImageDraw.Draw(image)
        draw.polygon([(12,30),(96,15),(115,31),(55,58),(115,94),(110,113),(20,72),(20,54),(73,33),(13,46)],fill=1)
        mask=np.asarray(image,dtype=bool)
        if reflected:mask=mask[:,::-1]
        return {'normalizedMask':self.encoded(mask),'bounds':[100,200,14,12],
                'right':{'top':{'xy':[113,207]},'bottom':{'xy':[114,209]}},
                'left':{'top':{'xy':[101,208]},'bottom':{'xy':[102,210]}}}

    def test_uniform_scale_and_translation_do_not_change_score(self):
        a=self.body();b=json.loads(json.dumps(a))
        b['bounds']=[a['bounds'][0]*2-30,a['bounds'][1]*2+40,28,24]
        for side in ['right','left']:
            for endpoint in ['top','bottom']:
                x,y=b[side][endpoint]['xy'];b[side][endpoint]['xy']=[2*x-30,2*y+40]
        x,y=trial.descriptor(a),trial.descriptor(b)
        self.assertAlmostEqual(trial.distance(x,y,np.ones(len(trial.FEATURE_NAMES)))[0],0)
        self.assertAlmostEqual(y['strokeInNativeUnits'],2*x['strokeInNativeUnits'])

    def test_chirality_is_preserved(self):
        a,b=trial.descriptor(self.body()),trial.descriptor(self.body(True))
        self.assertGreater(trial.distance(a,b,np.ones(len(trial.FEATURE_NAMES)))[0],.01)

    def test_missing_source_ink_rejected(self):
        source=np.ones((20,20),bool);one=np.zeros_like(source);one[:5]=True
        c={'partitionMasks':[self.encoded(one),self.encoded(one),self.encoded(one)]}
        check=trial.partition_check(c,source)
        self.assertFalse(check['valid']);self.assertGreater(check['missing'],0)

    def test_exact_partition_conserves_ink(self):
        source=np.ones((20,20),bool);masks=[]
        for lo,hi in [(0,5),(5,14),(14,20)]:
            mask=np.zeros_like(source);mask[lo:hi]=True;masks.append(self.encoded(mask))
        check=trial.partition_check({'partitionMasks':masks},source)
        self.assertTrue(check['valid']);self.assertEqual(check['overlap'],0)

    def test_two_hahs_use_ordered_distinct_candidates(self):
        a=np.zeros((10,20),bool);a[:,12:]=True;b=np.zeros_like(a);b[:,:8]=True
        common={'conservation':{'valid':True}}
        candidates=[dict(common,id=0,distance=.2,following=[1],mask=self.encoded(a)),
                    dict(common,id=1,distance=.1,following=[],mask=self.encoded(b))]
        ranked=trial.assignments(candidates,2)
        self.assertEqual(ranked[0][1],[0,1])
        self.assertEqual(trial.assignments(candidates,3),[])

    def test_train_loader_excludes_held_out_pages(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name);(folder/'candidates').mkdir()
            trial.write_json(folder/'inputs.json',{'pageSplits':{'page003':'train','page590':'validation','page600':'test'}})
            for page in (3,590,600):trial.write_json(folder/'candidates'/f'page{page:03}.json',{'page':page})
            self.assertEqual([d['page'] for d in trial.load_pages(folder,'train')],[3])

    def test_boundary_metric_tolerates_small_raster_offset(self):
        a=np.zeros((30,30),bool);a[8:20,8:20]=True;b=np.roll(a,1,axis=1)
        self.assertEqual(trial.boundary_f1(a,b,2),1)

if __name__=='__main__':unittest.main()
