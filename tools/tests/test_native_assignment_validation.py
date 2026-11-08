import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/mark-classifier'))
from audit_mark_assignments import read_native_assignment
from mark_validation import digest_json


class NativeValidationBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.transcript = {'pageId': 'page003', 'words': []}
        self.assignment = dict(pageId='page003', nativeAssignment=dict(
            producer='medinamushafanalyzer-native-marks-v1', rawInputSha256='raw',
            transcriptSha256=digest_json(self.transcript), geometrySha256='geometry'))

    def read(self, assignment):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'page003.assignment.json'
            path.write_text(json.dumps(assignment))
            return read_native_assignment(path, 'raw', self.transcript, 'geometry')

    def test_validation_reads_native_assignment_without_mutation(self):
        self.assertEqual(self.read(self.assignment), self.assignment)

    def test_missing_native_stage_never_falls_back_to_python_assignment(self):
        assignment = copy.deepcopy(self.assignment);del assignment['nativeAssignment']
        with self.assertRaisesRegex(ValueError, r'native C\+\+ assignment provenance'):
            self.read(assignment)

    def test_stale_sources_transcript_and_geometry_are_rejected(self):
        for field in ('rawInputSha256', 'transcriptSha256', 'geometrySha256'):
            with self.subTest(field=field):
                assignment = copy.deepcopy(self.assignment);assignment['nativeAssignment'][field] = 'stale'
                with self.assertRaisesRegex(ValueError, 'different source, transcript or geometry'):
                    self.read(assignment)

    def test_wrong_page_is_rejected(self):
        assignment = dict(self.assignment, pageId='page004')
        with self.assertRaisesRegex(ValueError, 'page differs'):
            self.read(assignment)
