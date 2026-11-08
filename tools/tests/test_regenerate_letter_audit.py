import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from letter_audit import regenerate_letter_audit as audit


class ResumeTests(unittest.TestCase):
    def test_valid_receipts_preserve_manifest_and_modified_export_is_rebuilt(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'pages').mkdir()
            (root / 'input').mkdir()
            (root / 'templates').mkdir()
            font = root / 'input/font.dat'
            font.write_bytes(b'font')
            (root / 'segmentsubword.cpp').write_bytes(b'cutter')
            executable = root / 'exporter'
            executable.write_bytes(b'export')
            snapshot = root / 'report'
            pages = snapshot / 'pages'
            pages.mkdir(parents=True)
            template = {'kashidaDefaults': {'trackingEnabled': False}}
            (root / 'templates/letter-forms.json').write_text(json.dumps(template))
            font_hash = audit.digest(font)

            def page_data(page):
                return {'page': page, 'sourceFileSha256': audit.digest(root / f'pages/page{page}.dat'),
                        'fontSha256': font_hash, 'templateConfig': template, 'subwords': [], 'sourceWords': []}

            for page in range(1, 605):
                (root / f'pages/page{page}.dat').write_bytes(str(page).encode())
                (pages / f'page{page:03}.json').write_text(json.dumps(page_data(page)))

            regenerated = []
            def export(command, **kwargs):
                page = int(command[-1])
                regenerated.append(page)
                (pages / f'page{page:03}.json').write_text(json.dumps(page_data(page)))
                return audit.subprocess.CompletedProcess(command, 0)

            with patch.object(audit, 'ROOT', root), patch.object(audit.subprocess, 'run', side_effect=export), \
                    contextlib.redirect_stdout(io.StringIO()):
                manifest = snapshot / 'generation.json'
                self.assertFalse(audit.run(snapshot, executable, jobs=1))
                # Adopt a legacy manifest, then keep its upgraded bytes stable on a warm resume.
                legacy = json.loads(manifest.read_bytes())
                del legacy['pageHashes']
                manifest.write_text(json.dumps(legacy))
                self.assertFalse(audit.run(snapshot, executable, jobs=1))
                self.assertEqual(len(json.loads(manifest.read_bytes())['pageHashes']), 604)
                verified = manifest.read_bytes()
                self.assertFalse(audit.run(snapshot, executable, jobs=1))
                self.assertEqual(manifest.read_bytes(), verified)
                self.assertEqual(regenerated, [])
                # Valid JSON with different cuts still needs regeneration.
                (pages / 'page372.json').write_text(json.dumps({**page_data(372), 'subwords': [{}]}))
                self.assertFalse(audit.run(snapshot, executable, jobs=1))
                self.assertEqual(regenerated, [372])
                self.assertEqual(json.loads((pages / 'page372.json').read_bytes()), page_data(372))
