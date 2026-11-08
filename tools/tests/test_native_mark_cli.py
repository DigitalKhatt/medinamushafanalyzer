import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT/'build/ninja-nmc/Release/medinamushafanalyzer'


class NativeReplayInputTests(unittest.TestCase):
    def setUp(self):
        fixture = json.loads((ROOT/'tools/tests/fixtures/mark-waqf-reservation.json').read_text())
        self.case = fixture['cases'][0]
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.doc = copy.deepcopy(self.case['native'])
        for path in self.doc['paths']:
            shape = self.case['geometry'].get(path['id'], {})
            path.update(signature=shape.get('signature', '0'), templateScores=shape.get('templateScores', {}))
        self.pid = self.doc['pageId']
        (self.root/'transcript.json').write_text(json.dumps(dict(
            producer='medinamushafanalyzer-QuranTextByWord-v1', pages=[self.case['transcript']]), ensure_ascii=False))

    def run_cli(self, output='assignments'):
        (self.root/f'{self.pid}.raw.json').write_text(json.dumps(self.doc, ensure_ascii=False))
        return subprocess.run([str(BINARY), '--assign-marks', str(self.root/output),
                               '--mark-inputs', str(self.root), '--pages', str(int(self.pid[4:]))],
                              cwd=ROOT, env={**os.environ, 'QT_QPA_PLATFORM': 'offscreen'},
                              capture_output=True, text=True, timeout=30)

    def test_embedded_and_separate_frozen_geometry_produce_identical_assignment(self):
        result = self.run_cli('embedded')
        self.assertEqual(result.returncode, 0, result.stderr)
        a = json.loads((self.root/'embedded'/f'{self.pid}.assignment.json').read_text())
        geometry = self.root/'geometry';geometry.mkdir()
        digest = hashlib.sha256((self.root/f'{self.pid}.raw.json').read_bytes()).hexdigest()
        (geometry/f'{self.pid}.json').write_text(json.dumps(dict(rawInputSha256=digest, paths=self.case['geometry'])))
        result = self.run_cli('separate')
        self.assertEqual(result.returncode, 0, result.stderr)
        b = json.loads((self.root/'separate'/f'{self.pid}.assignment.json').read_text())
        self.assertIsNone(a['nativeAssignment']['geometrySha256'])
        self.assertIsNotNone(b['nativeAssignment']['geometrySha256'])
        del a['nativeAssignment'];del b['nativeAssignment']
        self.assertEqual(a, b)

    def test_score_free_export_cannot_silently_skip_missing_geometry(self):
        for path in self.doc['paths']:
            path['templateScores'] = {}
        result = self.run_cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Score-free exports require frozen inputs/geometry', result.stderr)
        manifest = json.loads((self.root/'assignments/native-assignment-manifest.json').read_text())
        self.assertEqual(len(manifest['failures']), 1)
        self.assertEqual(manifest['pages'], [])
        self.assertFalse((self.root/'assignments'/f'{self.pid}.assignment.json').exists())

    def test_stale_geometry_is_rejected(self):
        geometry = self.root/'geometry';geometry.mkdir()
        (geometry/f'{self.pid}.json').write_text(json.dumps(dict(rawInputSha256='stale', paths=self.case['geometry'])))
        result = self.run_cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Frozen geometry belongs to different native paths', result.stderr)

    def test_mark_export_requires_template_scores_and_proposal_export(self):
        for flags in ([], ['--export-proposals', str(self.root), '--skip-template-scores']):
            result = subprocess.run([str(BINARY), '--export-mark-assignments', str(self.root), *flags],
                                    cwd=ROOT, env={**os.environ, 'QT_QPA_PLATFORM': 'offscreen'},
                                    capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('requires --export-proposals with template scores', result.stderr)
