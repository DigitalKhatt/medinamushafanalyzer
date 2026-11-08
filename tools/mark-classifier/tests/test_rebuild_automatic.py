import json
from pathlib import Path
import tempfile
import unittest

from rebuild_automatic import digest,empty_directory,export_models,validate_bundle,verify_rebuilds


class RebuildTests(unittest.TestCase):
    def test_repeatability_check_rejects_different_inputs_and_changed_cache_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            a,b=Path(directory)/'a',Path(directory)/'b';a.mkdir();b.mkdir()
            manifest={'status':'complete','sourceInputs':{'hash':'original'},'modelBundle':{},'codeSha256':{}}
            (a/'rebuild-manifest.json').write_text(json.dumps(manifest))
            (b/'rebuild-manifest.json').write_text(json.dumps({**manifest,'sourceInputs':{'hash':'changed'}}))
            with self.assertRaisesRegex(ValueError,'input snapshots'):verify_rebuilds(a,b)
            (b/'rebuild-manifest.json').write_text(json.dumps(manifest))
            (a/'features.u8').write_bytes(b'original');(b/'features.u8').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError,'features.u8'):verify_rebuilds(a,b)
    def test_current_reviews_take_precedence_over_a_published_frozen_snapshot(self):
        from automatic.common import input_root
        with tempfile.TemporaryDirectory() as directory:
            data=Path(directory);output=data/'automatic';(output/'inputs').mkdir(parents=True)
            (output/'inputs/corpus.json').write_text('{}')
            self.assertEqual(input_root(output),output/'inputs')
            (data/'corpus.json').write_text('{}')
            self.assertEqual(input_root(output),data)
    def test_existing_results_are_never_used_as_a_fresh_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output=Path(directory)/'output';empty_directory(output);(output/'corpus.sqlite').write_bytes(b'old cache')
            with self.assertRaisesRegex(ValueError,'nonempty'):empty_directory(output)
            self.assertEqual((output/'corpus.sqlite').read_bytes(),b'old cache')
    def test_model_export_removes_experiment_dependencies_and_pins_all_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)/'old';(root/'models').mkdir(parents=True)
            weights=root/'models/selected.npz';weights.write_bytes(b'frozen base')
            (root/'models/model.json').write_text(json.dumps({'modelSha256':digest(weights),'types':['damma','small-high-yeh']}))
            outside=Path(directory)/'experiment.npz';outside.write_bytes(b'specialist')
            (root/'models/hamza.json').write_text(json.dumps({'weights':str(outside),'weightsSha256':digest(outside),'baselineSha256':digest(weights)}))
            bundle=Path(directory)/'bundle';export_models(root,bundle)
            outside.unlink();validate_bundle(bundle)
            config=json.loads((bundle/'models/hamza.json').read_text());self.assertEqual(config['weights'],'models/hamza.npz')
            (bundle/'models/hamza.npz').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError,'changed'):validate_bundle(bundle)


if __name__=='__main__':unittest.main()
