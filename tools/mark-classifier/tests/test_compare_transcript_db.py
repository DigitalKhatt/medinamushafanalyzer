from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compare_transcript_db import compare_page


def native(text, ordinal=1, line=1):
    return dict(id=f'page003:l{line}-w{ordinal}', text=text, line=line, ordinal=ordinal)


def database(text, ordinal=1, line=1):
    return dict(id=f'page003:l{line}-w{ordinal}', dk_v2=text, line=line, ordinal=ordinal,
                word_number_all=ordinal, surah_number=2, ayah_number=8, word_number=ordinal)


class TranscriptDatabaseComparisonTests(unittest.TestCase):
    def test_maddah_spelling_is_preserved_and_kasra_is_not_normalized_away(self):
        result=compare_page([native('ٱلْأٓخِرِ')], [database('ٱلْـَٔاخِرِ')])
        self.assertEqual(len(result['spellingDifferences']),1)
        self.assertEqual(result['spellingDifferences'][0]['native'][0]['text'],'ٱلْأٓخِرِ')
        result=compare_page([native('بَ')],[database('بِ')])
        self.assertEqual(len(result['otherDifferences']),1)

    def test_standalone_hizb_does_not_shift_following_matches(self):
        result=compare_page([native('۞'),native('أَمْ',2),native('بَ',3)],
                            [database('۞أَمْ'),database('بَ',2)])
        self.assertEqual(result['nativeStandaloneHizb'],1)
        self.assertEqual(len(result['hizbWordAssociation']),1)
        self.assertEqual(result['otherDifferences'],[])
        self.assertEqual(result['wordBoundaries'],[])

    def test_split_words_and_database_internal_space_remain_boundary_differences(self):
        split=compare_page([native('لَّوْمَا')], [database('لَّوْ'),database('مَا',2)])
        joined=compare_page([native('إِلْ'),native('يَاسِينَ',2)], [database('إِلْ يَاسِينَ')])
        self.assertEqual(len(split['wordBoundaries']),1)
        self.assertEqual(len(joined['wordBoundaries']),1)
        self.assertEqual(joined['otherDifferences'],[])

    def test_line_movement_is_detected_when_text_is_identical(self):
        result=compare_page([native('مَا',line=12)],[database('مَا',line=13)])
        self.assertEqual(len(result['lineBoundaries']),1)
        self.assertEqual(result['otherDifferences'],[])

    def test_canonical_unicode_and_verse_prefix_are_separate(self):
        result=compare_page([native('أَمْ'),native('٦',2)],
                            [database('أَمْ'),database('۝٦',2)])
        self.assertEqual(len(result['canonicalUnicode']),1)
        self.assertEqual(len(result['verseMarkerPrefix']),1)
        self.assertEqual(result['otherDifferences'],[])


if __name__=='__main__':unittest.main()
