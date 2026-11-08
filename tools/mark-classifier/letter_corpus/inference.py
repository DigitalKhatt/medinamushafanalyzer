"""Portable, image-only inference with the recent comparison's frozen model."""
import json
from pathlib import Path
import shutil

from automatic.common import digest_file
from .geometry import fingerprint, validate_partition


def prepare_bundle(experiment, root):
    """Keep config and trained weights together; no /tmp dependency after bundling."""
    bundle = root/'model'
    bundle.mkdir(parents=True, exist_ok=True)
    source = experiment/'mask2former'
    settings = json.loads((source/'settings.json').read_text())
    model_sha = digest_file(source/'best.pt')
    manifest_path = bundle/'manifest.json'
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text())
        if manifest['modelSha256'] != model_sha:
            raise ValueError('This corpus uses another checkpoint; create a new corpus directory')
        for name, sha in manifest['files'].items():
            if digest_file(bundle/name) != sha:
                raise ValueError(f'Changed bundled model file: {name}')
        return manifest
    config = Path(settings['checkpoint'])/'config.json'
    if not config.exists():
        raise ValueError(f'Missing model architecture configuration: {config}')
    shutil.copyfile(source/'best.pt', bundle/'best.pt')
    shutil.copyfile(config, bundle/'config.json')
    shutil.copyfile(source/'settings.json', bundle/'settings.json')
    manifest = dict(method='mask2former', modelSha256=model_sha,
        origin=str(source.resolve()), trainingStep=settings['step'], threshold=settings['threshold'],
        initialization=settings.get('initialization','pretrained'),
        canvas=settings['canvas'], datasetSha256=settings['datasetSha256'],
        files={n: digest_file(bundle/n) for n in ('best.pt', 'config.json', 'settings.json')})
    manifest_path.write_text(json.dumps(manifest, indent=2)+'\n')
    return manifest


class Predictor:
    def __init__(self, root, device='cpu'):
        self.root, self.device, self.model = Path(root), device, None
        self.bundle = self.root/'model'
        self.settings = json.loads((self.bundle/'manifest.json').read_text())
        for name, sha in self.settings['files'].items():
            if digest_file(self.bundle/name) != sha:
                raise ValueError(f'Changed model file: {name}')
        self.code_sha = fingerprint({n: digest_file(Path(__file__).parents[1]/n)
                                    for n in ('letter_retest.py', 'letter_page_compare.py',
                                              'letter_corpus/inference.py')})

    def get(self, ink, box, page_width):
        import numpy as np
        key = fingerprint(dict(model=self.settings['modelSha256'], threshold=self.settings['threshold'],
            canvas=self.settings['canvas'], code=self.code_sha, shape=list(ink.shape),
            ink=np.packbits(ink).tobytes().hex(), device=self.device))
        folder = self.root/'predictions'
        folder.mkdir(exist_ok=True)
        path = folder/f'{key}.npz'
        if path.exists():
            with np.load(path, allow_pickle=False) as f:
                result = {k: f[k].copy() for k in ('instances', 'roles', 'raw')}
        else:
            # Use exactly the recent whole-page comparison's input preparation,
            # trained checkpoint and decoder. No transcript/template is passed.
            import torch
            from transformers import Mask2FormerConfig, Mask2FormerForUniversalSegmentation
            from letter_retest import predict
            from letter_page_compare import inference_example
            if self.model is None:
                torch.set_num_threads(4)
                torch.manual_seed(63)
                config = Mask2FormerConfig.from_pretrained(str(self.bundle), local_files_only=True)
                config.num_labels = 2
                config.id2label = {0: 'body', 1: 'kashida'}
                config.label2id = {'body': 0, 'kashida': 1}
                config.train_num_points = 2048
                config.use_auxiliary_loss = True
                self.model = Mask2FormerForUniversalSegmentation(config)
                self.model.load_state_dict(torch.load(self.bundle/'best.pt', map_location='cpu', weights_only=True), strict=True)
                self.model.to(torch.device(self.device)).eval()
            result0 = predict(self.model, 'mask2former', [inference_example({'id': 'source-ink'}, ink)],
                              torch.device(self.device), self.settings['threshold'])[0]
            result = dict(instances=result0['prediction'], roles=result0['roles'], raw=result0['raw'])
            np.savez_compressed(path, **result)
        validate_partition(ink, result['instances'], result['raw'])
        result['key'] = key
        return result
