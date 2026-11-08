"""Optional ML export and evaluation invariants; normal workbench stays stdlib-only."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

AVAILABLE = all(importlib.util.find_spec(x) is not None for x in ('torch','scipy','skimage','numpy'))
if AVAILABLE:
    import numpy as np
    from letter_retest_data import normalize_partition, weak_label_exclusion
    from letter_retest import boundary, load_dataset, metric, summarize, mask_decode, seg_decode, minimum_region_area


@unittest.skipUnless(AVAILABLE, 'Optional letter-model dependencies are not installed')
class LetterRetestTests(unittest.TestCase):
    def test_cutter_only_dataset_rejects_reviewed_targets_and_excluded_crops(self):
        weak = dict(id='other', provenance='machine-weak', split='train', group='weak_train')
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            for record in (
                dict(weak, provenance='human-reviewed'),
                dict(weak, id='excluded'),
                dict(weak, group='human_train'),
            ):
                (folder/'dataset.json').write_text(json.dumps(
                    dict(excludedReviewedIds=['excluded'], records=[record])))
                with self.assertRaisesRegex(ValueError, 'Cutter-only dataset contains an ineligible crop'):
                    load_dataset(folder, for_training=True)

    def test_reviewed_body_fragments_with_one_owner_count_as_one_base(self):
        from unittest.mock import patch
        from letter_retest_data import human_record
        labels = np.array([[1, 2, 3]])
        annotation = dict(revision=2, sourceSha256='source', raster={}, groups={'c1': {'label': 'م'}},
                          parts=[dict(role=role, owner='c1') for role in ('body', 'body', 'kashida')])
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp)
            (data/'annotations').mkdir()
            path = data/'annotations/page001.json'
            path.write_text(json.dumps(annotation))
            with patch('letter_retest_data.reviewed_partition', return_value=([0, 0, 3, 1], labels)):
                record, _, roles = human_record(data, 'page001', 'p', 1, 'id', 'train')
                self.assertEqual(roles.tolist(), [0, 1, 1, 2])
                self.assertEqual(record['baseCharacterCount'], 1)
                annotation['groups']['c2'] = {'label':'م'}
                annotation['parts'][1]['owner'] = 'c2'
                path.write_text(json.dumps(annotation))
                self.assertEqual(human_record(data, 'page001', 'p', 1, 'id', 'train')[0]['baseCharacterCount'], 2)

    @unittest.skipUnless(importlib.util.find_spec('transformers'), 'Optional transformers dependency is absent')
    def test_random_model_uses_only_config_and_reproduces_initial_weights(self):
        from unittest.mock import patch
        import torch
        from transformers import AutoBackbone, Mask2FormerConfig, Mask2FormerForUniversalSegmentation, SwinConfig
        from letter_retest import model_for,state_digest
        backbone=SwinConfig(embed_dim=16,depths=[1,1,1,1],num_heads=[1,2,4,8],
                            out_features=['stage1','stage2','stage3','stage4'])
        config=Mask2FormerConfig(backbone_config=backbone.to_dict(),feature_size=32,mask_feature_size=32,
            hidden_dim=32,encoder_layers=1,decoder_layers=2,num_attention_heads=4,
            dim_feedforward=64,encoder_feedforward_dim=64,num_queries=5)
        with tempfile.TemporaryDirectory() as tmp:
            config.save_pretrained(tmp)  # Deliberately no checkpoint weight file.
            with patch.object(Mask2FormerForUniversalSegmentation,'from_pretrained',side_effect=AssertionError('Pretrained weights read')), \
                 patch.object(AutoBackbone,'from_pretrained',side_effect=AssertionError('Pretrained backbone read')):
                torch.manual_seed(63);first=model_for('mask2former',tmp,torch.device('cpu'),'random')
                torch.manual_seed(63);again=model_for('mask2former',tmp,torch.device('cpu'),'random')
                self.assertEqual(state_digest(first),state_digest(again))
                torch.manual_seed(64);changed=model_for('mask2former',tmp,torch.device('cpu'),'random')
                self.assertNotEqual(state_digest(first),state_digest(changed))
                self.assertTrue(all(p.requires_grad for p in first.parameters()))
                self.assertFalse(first.config.use_pretrained_backbone)
                self.assertEqual(first.config.num_labels,2)
                self.assertEqual([p.name for p in Path(tmp).iterdir()],['config.json'])

    def test_provisional_labels_cannot_enter_automatic_training(self):
        for marker in ({'provisional': True}, {'letters': [{'status': 'remainder'}]},
                       {'regions': [{'labelSource': 'remainder'}]}):
            for complete in (False, True):
                self.assertEqual(weak_label_exclusion(dict(marker, complete=complete)), 'provisional-remainder')
        self.assertEqual(weak_label_exclusion({'complete': False}), 'partial')
        self.assertIsNone(weak_label_exclusion({'complete': True}))

    def test_reject_large_hole_in_targets(self):
        source=np.ones((20,20),bool);part=source.copy();part[:,8:12]=False
        with self.assertRaisesRegex(ValueError,'ink-conservation'):
            normalize_partition(source,[part],[1])

    def test_source_ink_and_roles_survive_subpixel_conflict(self):
        source=np.pad(np.ones((10,10),bool),2)
        a=source.copy();a[:,7:]=False;b=source.copy();b[:,:7]=False
        b[4,6]=True
        labels,roles,n,_=normalize_partition(source,[a,b],[1,2])
        self.assertTrue(np.array_equal(labels>0,source))
        self.assertEqual(roles.tolist(),[0,1,2]);self.assertEqual(n,1)

    def test_metric_is_instance_permutation_invariant(self):
        a=np.array([[0,1,1,2],[0,1,1,2]])
        b=np.array([[0,8,8,3],[0,8,8,3]])
        result=summarize([metric(a,b)])
        self.assertEqual(result['instanceF1AtIoU50'],1)
        self.assertEqual(result['meanIoUPerReference'],1)

    def test_missed_and_extra_regions_are_penalized(self):
        a=np.array([[1,1,2,2],[1,1,2,2]])
        merged=np.ones_like(a)
        self.assertLess(summarize([metric(a,merged)])['instanceF1AtIoU50'],1)
        self.assertEqual(metric(a,merged)['absoluteCountError'],1)

    def test_boundary_does_not_wrap_at_edges(self):
        a=np.array([[1,1,0,2,2],[1,1,0,2,2]])
        self.assertFalse(boundary(a).any())

    def test_mask_decoder_abstains_without_confident_query(self):
        ink=np.ones((10,10),bool)
        result,_,_=mask_decode(np.ones((2,10,10)),np.array([[.1,.1,.8],[.1,.2,.7]]),ink)
        self.assertFalse(result.any())

    def test_tooth_cutoff_filters_instances_and_conserves_source_ink(self):
        ink=np.ones((4,8),bool)
        masks=np.zeros((2,4,8));masks[0,:,:6]=1;masks[1,:,6:]=1
        classes=np.array([[.9,.05,.05],[.05,.9,.05]])
        pred,roles,raw=mask_decode(masks,classes,ink,min_region_area=9)
        self.assertEqual(roles.tolist(),[0,1])
        self.assertEqual(int((raw>0).sum()),24)
        self.assertTrue(np.array_equal(pred>0,ink))
        self.assertFalse((raw[:,6:]>0).any())

    def test_all_small_predictions_leave_explicit_unknown_ink(self):
        ink=np.ones((2,4),bool)
        pred,roles,raw=mask_decode(np.ones((1,2,4)),np.array([[.9,.05,.05]]),ink,min_region_area=9)
        self.assertFalse(pred.any());self.assertFalse(raw.any());self.assertEqual(roles.tolist(),[0])

    def test_tooth_area_scales_with_exact_page_transform(self):
        policy=dict(fraction=.1,meanToothAnalyzerArea=10,referenceRasterAreaScale=16)
        self.assertEqual(minimum_region_area(policy),16)
        self.assertEqual(minimum_region_area(policy,{'pdfToRaster':[5,0,0,6,0,0]}),30)

    def test_both_decoders_preserve_counters(self):
        ink=np.pad(np.ones((10,10),bool),2);ink[5:8,5:8]=False
        masks=np.ones((1,*ink.shape))
        pred,_,_=mask_decode(masks,np.array([[.9,.05,.05]]),ink)
        self.assertTrue(np.array_equal(pred>0,ink))
        probabilities=np.zeros((4,*ink.shape));probabilities[1]=1
        pred,roles,_=seg_decode(probabilities,ink)
        self.assertTrue(np.array_equal(pred>0,ink));self.assertEqual(roles.tolist(),[0,1])


if __name__=='__main__':unittest.main()
