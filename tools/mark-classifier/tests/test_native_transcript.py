import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from automatic.native_transcript import subword_characters, validate_snapshot


class NativeTranscriptTests(unittest.TestCase):
    def fixture(self):
        source='\n'.join(f'R"page{n}(\nبَ\n)page{n}"' for n in range(604))
        pages=[dict(pageId=f'page{n+1:03}',rawText='\nبَ\n',words=[]) for n in range(604)]
        pages[219]['words']=[dict(id='page220:l3-w8',line=3,ordinal=8,text='لَأٓمَنَ',
            characters=[dict(text=c,position='unknown') for c in 'لَأٓمَنَ'])]
        return source,dict(producer='medinamushafanalyzer-QuranTextByWord-v1',pages=pages)

    def test_current_native_snapshot_preserves_codepoints_and_indices(self):
        source,snapshot=self.fixture()
        words=validate_snapshot(snapshot,source)
        self.assertEqual(words['page220']['page220:l3-w8']['text'],'لَأٓمَنَ')
        self.assertIn('ٓ',[c['text'] for c in words['page220']['page220:l3-w8']['characters']])

    def test_rejects_stale_binary_missing_pages_and_invalid_character_records(self):
        source,snapshot=self.fixture()
        for edit in ('raw','page','characters'):
            changed=copy.deepcopy(snapshot)
            if edit=='raw':changed['pages'][0]['rawText']='stale'
            elif edit=='page':changed['pages'].pop()
            else:changed['pages'][219]['words'][0]['characters'].pop()
            with self.assertRaises(ValueError):validate_snapshot(changed,source)

    def test_subwords_use_native_characters_and_do_not_reinterpret_maddah(self):
        word=dict(text='لَأٓمَنَ',characters=[dict(text=c,position='unknown') for c in 'لَأٓمَنَ'])
        subs=[dict(id='s1',text='لَأٓ'),dict(id='s2',text='مَنَ')]
        result=subword_characters(word,subs)
        self.assertEqual(''.join(c['text'] for c in result['s1']),'لَأٓ')
        self.assertEqual(subword_characters(word,[dict(id='bad',text='لَأَمَنَ')]),{})
        with_sign=dict(word,text=word['text']+'۩',characters=word['characters']+[dict(text='۩',position='unknown')])
        self.assertEqual(subword_characters(with_sign,subs),result)
        with_mark=dict(word,text=word['text']+'ِ',characters=word['characters']+[dict(text='ِ',position='unknown')])
        self.assertEqual(subword_characters(with_mark,subs),{})


if __name__=='__main__':unittest.main()
