"""Letter corpus invariants: source conservation, abstention, and review survival."""
import importlib.util
import copy
from contextlib import closing
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

NUMERICAL = all(importlib.util.find_spec(n) for n in ('numpy','scipy'))


@unittest.skipUnless(NUMERICAL, 'Numerical corpus builder is optional; server remains stdlib-only')
class LetterCorpusTests(unittest.TestCase):
    def setUp(self):
        import numpy as np
        from letter_corpus.geometry import encode_pixels, bounds
        from letter_corpus.build import build_page, publish_page
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.width, self.height = 40, 26
        self.sid = 'page003:l1-w1-s1'
        self.word = 'page003:l1-w1'
        body = np.zeros((26,40),bool)
        body[10:16,22:29] = True; body[10:16,9:16] = True; body[13:15,16:22] = True
        def item(name, mask, kind, subtype):
            runs = encode_pixels(np.flatnonzero(mask));x,y,w,h=bounds(runs,self.width)
            return dict(id=name,word_id=self.word,subword_id=self.sid,line=1,x=x,y=y,w=w,h=h,
                runs=runs,area=int(mask.sum()),origin='machine-weak',human_reviewed=0,
                kind=kind,mark_type=subtype,teacher_kind=kind,teacher_type=subtype,
                final_kind=kind,final_type=subtype,model_confidence=.99)
        items = [item('body',body,'subword-ink','unknown')]
        for name,x,y,kind,subtype in [('dot1',25,19,'dot','single-dot'),('dot2',12,7,'dot','double-dot'),
                                    ('fatha',12,3,'mark','fatha'),('kasra',25,23,'mark','kasra')]:
            ink=np.zeros_like(body);ink[y,x:x+2]=True;items.append(item(name,ink,kind,subtype))
        chars=[dict(text=c,position=p) for c,p in [('ب','initial'),('ِ','unknown'),('ت','final'),('َ','unknown')]]
        self.snapshot=dict(page=dict(page_id='page003',split='train',width=40,height=26,source_sha='source'),
            words=[dict(id=self.word,page_id='page003',line=1,ordinal=1,text='بِتَ',bases='بت',x=9,y=3,w=20,h=21)],
            subwords=[dict(id=self.sid,page_id='page003',word_id=self.word,text='بِتَ',x=9,y=10,w=20,h=6)],
            items=items,transcript={self.word:dict(text='بِتَ',characters=chars)},provenance={'test':'source fixture'})
        class Predictor:
            def get(_, ink, box, width):
                labels=np.zeros(ink.shape,'uint16')
                yy,xx=np.indices(ink.shape);xx=xx+box[0]
                labels[ink & (xx>=22)]=1;labels[ink & (xx<16)]=2;labels[ink & (xx>=16)&(xx<22)]=3
                raw=labels.copy();raw[np.where(raw>0)[0][0],np.where(raw>0)[1][0]]=0
                return dict(instances=labels,raw=raw,roles=np.array([0,1,1,2]),key='fixture')
        self.predictor=Predictor()
        self.records=build_page(self.snapshot,self.predictor)
        self.model=dict(method='mask2former',modelSha256='fixture',trainingStep=1800,threshold=.35)
        publish_page(self.root/'corpus.sqlite',self.records,self.model)
        from letter_corpus.query import Corpus
        self.corpus=Corpus(self.root)

    def tearDown(self): self.tmp.cleanup()

    def single_base_snapshot(self):
        import numpy as np
        from letter_corpus.geometry import encode_pixels, bounds
        snapshot=copy.deepcopy(self.snapshot)
        for row in snapshot['words']+snapshot['subwords']: row.update(text='ضِ',bases='ض')
        snapshot['transcript'][self.word]=dict(text='ضِ',characters=[
            dict(text='ض',position='isolated'),dict(text='ِ',position='unknown')])
        # One letter with two detached native body contours, including a hole.
        # These must remain one unchanged body, without an invented connection.
        parts=[]
        for number,(left,right) in enumerate([(9,16),(22,29)]):
            ink=np.zeros((self.height,self.width),bool);ink[10:16,left:right]=True
            if number==0: ink[11:15,10:15]=False
            item=copy.deepcopy(snapshot['items'][0])
            runs=encode_pixels(np.flatnonzero(ink));x,y,w,h=bounds(runs,self.width)
            item.update(id=f'body{number}',runs=runs,x=x,y=y,w=w,h=h,area=int(ink.sum()))
            parts.append(item)
        snapshot['items']=parts+[i for i in snapshot['items'] if i['id'] in ('dot1','kasra')]
        return snapshot

    def test_single_base_bypasses_predictor_and_keeps_original_body_contours(self):
        from unittest.mock import Mock
        from automatic.common import unpack_runs
        from letter_corpus.build import build_page
        from letter_corpus.geometry import encode_pixels,pixels
        snapshot=self.single_base_snapshot()
        predictor=Mock()
        predictor.get.side_effect=AssertionError('Single-base body must never call the segmenter')
        records=build_page(snapshot,predictor)
        predictor.get.assert_not_called()
        self.assertEqual(len(records['letters']),1)  # Kasra is not a second base.
        self.assertEqual(len(records['regions']),1)
        region=records['regions'][0]
        expected=encode_pixels([p for i in snapshot['items'] if i['id'].startswith('body') for p in pixels(i['runs'])])
        self.assertEqual(unpack_runs(region['runs']),expected)
        self.assertEqual(region['raw_runs'],region['runs'])
        self.assertEqual(region['role'],'body')
        self.assertEqual(region['letter_id'],self.sid+'-letter1')
        sub=records['subwords'][0];diagnostics=json.loads(sub['diagnostics'])
        self.assertEqual(sub['alignment'],'single-base-source')
        self.assertEqual(sub['status'],'proposed')
        self.assertFalse(diagnostics['modelApplied'])
        self.assertIsNone(diagnostics['predictionKey'])
        self.assertEqual(diagnostics['unassignedPixels'],0)
        self.assertTrue(all(m['predicted_letter_id']==region['letter_id'] for m in records['marks']))

    def test_missing_or_misaligned_native_transcript_cannot_trigger_bypass(self):
        from unittest.mock import Mock
        from letter_corpus.build import build_page
        for mismatch in (False,True):
            with self.subTest(misaligned=mismatch):
                snapshot=self.single_base_snapshot()
                if mismatch: snapshot['transcript'][self.word]['text']='سِ'
                else: snapshot['transcript']={}
                predictor=Mock(wraps=self.predictor)
                records=build_page(snapshot,predictor)
                predictor.get.assert_called_once()
                self.assertFalse(records['letters'])
                sub=records['subwords'][0]
                self.assertEqual(sub['status'],'unresolved')
                self.assertNotEqual(sub['alignment'],'single-base-source')
                self.assertFalse(json.loads(sub['diagnostics'])['transcriptAligned'])

    def test_partition_keeps_every_source_pixel_and_no_mark_enters_body(self):
        from automatic.common import unpack_runs
        from letter_corpus.geometry import pixels
        source=set(pixels(self.snapshot['items'][0]['runs']))
        accumulated=set()
        for region in self.records['regions']:
            piece=set(pixels(unpack_runs(region['runs'])))
            self.assertFalse(accumulated & piece);accumulated |= piece
        self.assertEqual(accumulated,source)
        self.assertEqual([l['char'] for l in self.records['letters']],['ب','ت'])
        self.assertEqual(self.records['letters'][0]['x'],22)
        self.assertEqual(sum(r['raw_area'] for r in self.records['regions']),len(source)-1)
        self.assertEqual(next(r for r in self.records['regions'] if r['role']=='kashida')['letter_id'],self.sid+'-letter1')

    def test_count_mismatch_keeps_letter_owners_unresolved(self):
        from letter_corpus.alignment import align_bodies
        import numpy as np
        owners,reason=align_bodies(self.records['letters'],[dict(id='one',mask=np.ones((5,5),bool))])
        self.assertEqual(owners,{});self.assertEqual(reason,'count-mismatch')

    def test_stacked_equal_centers_do_not_get_an_arbitrary_order(self):
        from letter_corpus.alignment import align_bodies
        import numpy as np
        owners,reason=align_bodies(self.records['letters'],[dict(id=str(i),mask=np.ones((5,5),bool)) for i in range(2)])
        self.assertEqual(owners,{});self.assertEqual(reason,'ambiguous-order')

    def test_mark_assignment_has_unique_slots_and_keeps_type_review_separate(self):
        marks={m['id']:m for m in self.records['marks']}
        self.assertEqual(marks['dot1']['predicted_letter_id'],self.sid+'-letter1')
        self.assertEqual(marks['fatha']['predicted_letter_id'],self.sid+'-letter2')
        self.assertEqual(marks['fatha']['method'],'transcript-quota-and-geometry')
        self.assertEqual(marks['fatha']['type_reviewed'],0)
        self.assertIsNone(marks['fatha']['type'])
        self.assertIsNone(marks['fatha']['type_confidence'])

    def test_learned_mark_labels_and_confidences_cannot_change_the_corpus(self):
        from letter_corpus.build import build_page
        changed=copy.deepcopy(self.snapshot)
        for i in changed['items']:
            i.update(final_kind='symbol',final_type='wrong-label',model_confidence=0.,teacher_type='irrelevant')
        rebuilt=build_page(changed,self.predictor)
        for table in self.records.keys()-{'pages'}:
            self.assertEqual(self.records[table],rebuilt[table],table)

    def test_alif_body_recovery_uses_geometry_without_classifier(self):
        from letter_corpus.build import source_roles
        items=[dict(id='stem',subword_id='s',h=30,w=4,teacher_kind='unknown',human_reviewed=0),
               dict(id='hamza',subword_id='s',h=8,w=9,teacher_kind='subword-ink',human_reviewed=0)]
        roles,adjustments=source_roles(dict(items=items,subwords=[dict(id='s',text='إِ')]))
        self.assertEqual(roles,{'stem':'subword-ink','hamza':'mark'})
        self.assertEqual(len(adjustments),2)
        items[0].update(human_reviewed=1,kind='unknown')
        roles,adjustments=source_roles(dict(items=items,subwords=[dict(id='s',text='إِ')]))
        self.assertFalse(adjustments)

    def test_duplicate_mark_inventory_does_not_force_an_assignment(self):
        from letter_corpus.alignment import assign_marks
        result=assign_marks([dict(id='a',kind='mark'),dict(id='b',kind='mark')],
                            [dict(type='kasra',letter_id='one')],{})
        self.assertTrue(all(m['status']=='unresolved' and m['method']=='mark-count-mismatch' for m in result.values()))

    def test_repeated_types_match_one_to_one_and_ties_abstain(self):
        from letter_corpus.alignment import assign_marks
        slots=[dict(type='fatha',letter_id=i,side='above',mark_id=None) for i in ('right','left')]
        marks=[dict(id='a',kind='mark',x=40,y=0,w=2,h=2),dict(id='b',kind='mark',x=10,y=0,w=2,h=2)]
        anchors={i:dict(x=x,y=10,w=10,h=10) for i,x in [('right',40),('left',10)]}
        result=assign_marks(marks,slots,anchors)
        self.assertEqual([result[i]['predicted_letter_id'] for i in ('a','b')],['right','left'])
        marks[1]['x']=40
        result=assign_marks(marks,slots,anchors)
        self.assertTrue(all(m['status']=='unresolved' for m in result.values()))

    def test_mark_permutations_on_same_letter_are_not_ownership_ambiguity(self):
        from letter_corpus.alignment import assign_marks
        result=assign_marks([dict(id='a',kind='mark'),dict(id='b',kind='mark')],
            [dict(type=t,letter_id='one') for t in ('fatha','shadda')],{})
        self.assertTrue(all(m['predicted_letter_id']=='one' for m in result.values()))

    def test_literal_alif_hamza_maddah_equivalence_and_word_sign(self):
        from letter_corpus.alignment import letter_records,expected_slots
        chars=[dict(text=c,position='isolated' if c=='أ' else 'unknown') for c in 'أٓۖ']
        sub=self.snapshot['subwords'][0]
        letters,owners=letter_records(sub,chars,0,0)
        slots=expected_slots(sub,chars,owners)
        self.assertEqual([s['type'] for s in slots],['hamza-above','fatha','waqf-sad'])
        self.assertEqual(slots[1]['letter_id'],letters[0]['id']);self.assertIsNone(slots[2]['letter_id'])
        self.assertEqual(slots[1]['source_character'],'ٓ')
        self.assertEqual(slots[1]['character'],'َ')

    def test_each_ink_mask_can_name_its_transcript_character_or_dots(self):
        mapped={r['mark_id']:r for r in self.records['mark_transcripts']}
        self.assertEqual(mapped['dot1']['expected_type'],'single-dot')
        self.assertEqual(mapped['dot1']['source_character'],'ب')
        self.assertEqual(mapped['dot1']['dot_count'],1)
        self.assertIsNone(mapped['dot1']['character'])
        self.assertEqual(mapped['fatha']['character'],'َ')
        self.assertEqual(mapped['fatha']['word_text_offset'],3)
        slots=[r['slot_id'] for r in mapped.values() if r['slot_id']]
        self.assertEqual(len(slots),len(set(slots)))

    def test_transcript_type_ambiguity_does_not_erase_known_letter_owner(self):
        from letter_corpus.mark_transcript import associate
        marks=copy.deepcopy([r for r in self.records['marks'] if r['kind']=='mark'])
        owner=self.sid+'-letter1'
        for m in marks:m['predicted_letter_id']=owner
        slots=[dict(id=str(i),letter_id=owner,type=t,side='above',character=c,source_character=c,
                    word_text_offset=i,dot_count=None,mark_id=None) for i,(t,c) in enumerate([('fatha','َ'),('shadda','ّ')])]
        anchors={owner:dict(x=22,y=10,w=7,h=6)}
        result=associate(marks,slots,anchors,self.records['source_items'],self.width)
        self.assertTrue(all(r['status']=='ambiguous-transcript-mark' for r in result))
        self.assertTrue(all(m['predicted_letter_id']==owner for m in marks))

    def test_classifier_validation_cannot_rewrite_transcript_associations(self):
        from validate_letter_corpus_marks import validate
        source=self.root/'classifier.sqlite'
        with closing(sqlite3.connect(source)) as db,db:
            db.executescript('CREATE TABLE meta(key TEXT,value TEXT); CREATE TABLE items(id TEXT,runs BLOB,prediction_kind TEXT,prediction_type TEXT,model_confidence REAL);')
            for s in self.records['source_items']:
                if s['id']=='body':continue
                db.execute('INSERT INTO items VALUES (?,?,?,?,?)',(s['id'],s['runs'],s['kind'],'deliberately-wrong',.999))
        with closing(sqlite3.connect(self.corpus.path)) as db:
            before={t:db.execute(f'SELECT * FROM {t}').fetchall() for t in ('letters','marks','regions','mark_transcripts')}
        report=validate(self.root,source)
        self.assertEqual(report['counts']['difference'],4)
        with closing(sqlite3.connect(self.corpus.path)) as db:
            for table,rows in before.items():self.assertEqual(db.execute(f'SELECT * FROM {table}').fetchall(),rows)

    def test_review_survives_reindex_and_conflicts_do_not_overwrite_it(self):
        from letter_corpus.build import publish_page
        from letter_corpus.query import Conflict
        mark=next(m for m in self.corpus.detail('marks','fatha')['marks'] if m['id']=='fatha')
        payload=dict(id='fatha',fingerprint=mark['fingerprint'],revision=0,
                     state='approved',letter_id=self.sid+'-letter1',note='Reviewed physical location')
        self.corpus.review(payload)
        publish_page(self.root/'corpus.sqlite',self.records,self.model)
        with self.assertRaises(Conflict): self.corpus.review(payload)
        m=next(m for m in self.corpus.detail('marks','fatha')['marks'] if m['id']=='fatha')
        self.assertTrue(m['attachment_reviewed']);self.assertEqual(m['owner'],self.sid+'-letter1')
        self.assertEqual(m['type_reviewed'],0)
        result=self.corpus.search({'kind':'letters','type':'fatha'})
        self.assertEqual([r['id'] for r in result['rows']],[self.sid+'-letter2'])  # Transcript filter.
        result=self.corpus.search({'kind':'marks','type':'kasra'})
        self.assertIn('fatha',[r['id'] for r in result['rows']])  # Reviewed owner, no type classifier.
        with closing(sqlite3.connect(self.corpus.reviews)) as db:self.assertEqual(db.execute('SELECT COUNT(*) FROM review_events').fetchone()[0],1)

    def test_changed_segmentation_reopens_effective_review(self):
        m=next(m for m in self.corpus.detail('marks','fatha')['marks'] if m['id']=='fatha')
        self.corpus.review(dict(id='fatha',fingerprint=m['fingerprint'],revision=0,state='approved',letter_id=self.sid+'-letter1'))
        with closing(sqlite3.connect(self.corpus.path)) as db, db:db.execute("UPDATE marks SET fingerprint='changed' WHERE id='fatha'")
        m=next(m for m in self.corpus.detail('marks','fatha')['marks'] if m['id']=='fatha')
        self.assertFalse(m['attachment_reviewed']);self.assertTrue(m['review_stale'])
        self.assertEqual(m['owner'],self.sid+'-letter2')

    def test_cross_subword_review_and_wrong_source_are_rejected(self):
        from letter_corpus.query import Conflict
        m=next(m for m in self.corpus.detail('marks','fatha')['marks'] if m['id']=='fatha')
        with self.assertRaises(ValueError):self.corpus.review(dict(id='fatha',fingerprint=m['fingerprint'],revision=0,state='approved',letter_id='other-word'))
        with self.assertRaises(Conflict):self.corpus.review(dict(id='fatha',fingerprint='stale',revision=0,state='approved',letter_id=None))

    def test_search_detail_and_images_do_not_mutate_predictions(self):
        from automatic.common import digest_file
        before=digest_file(self.corpus.path)
        self.assertEqual(self.corpus.search({'kind':'letters','char':'ب','form':'initial'})['total'],1)
        self.assertEqual(self.corpus.search({'kind':'subwords','q':'بت'})['total'],1)
        self.assertEqual(self.corpus.search({'kind':'marks','type':'double-dot'})['total'],1)
        svg=self.corpus.image('letters',self.sid+'-letter1',context=True,raw=True)
        self.assertIn('<svg',svg);self.assertIn('<path',svg)
        self.assertEqual(digest_file(self.corpus.path),before)
        self.assertEqual(self.corpus.metadata()['counts']['letters'],2)

    def test_different_checkpoint_cannot_mix_into_same_corpus(self):
        from letter_corpus.build import publish_page
        with self.assertRaises(ValueError):publish_page(self.corpus.path,self.records,dict(self.model,modelSha256='other'))

    def test_model_partition_rejects_extra_ink_or_changed_raw_instances(self):
        from letter_corpus.geometry import validate_partition
        import numpy as np
        ink=np.zeros((5,5),bool);ink[2,2]=True
        labels=np.ones((5,5),int)
        with self.assertRaises(ValueError):validate_partition(ink,labels,labels)
        labels=np.where(ink,1,0);raw=np.where(ink,2,0)
        with self.assertRaises(ValueError):validate_partition(ink,labels,raw)


if __name__=='__main__':unittest.main()
