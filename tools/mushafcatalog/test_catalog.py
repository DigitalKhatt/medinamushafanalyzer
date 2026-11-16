"""Small native-format fixtures. These tests run without models, exported pages or an existing corpus."""
from contextlib import closing
from copy import deepcopy
from io import StringIO
import json
from pathlib import Path
import sqlite3
from tempfile import TemporaryDirectory
import unittest

from search import body_source, detail, image, search
from store import ROOT, build, encoded, export_bodies, import_bodies, read, sha, verify
from automatic.transcript_marks import replay_existing_index
import input_lock  # Exposes the mark-classifier modules to the catalog.
from check_fresh_mark_regression import difference_count_errors

PID = 'page003'
WID = PID+':l1-w1'
S1, S2 = WID+'-s1', WID+'-s2'
O0, O2 = WID+':c0', WID+':c2'


def write_fixture(root, with_long_run=False, source_sha='pdf'):
    root.mkdir()
    (root/'inputs').mkdir()
    wid2 = PID+':l1-w2'
    text = 'بِرَأ'
    word = dict(id=WID,line=1,ordinal=1,text=text,characters=[
        dict(text=ch,position=form) for ch,form in zip(text,['initial','unknown','final','unknown','isolated'])])
    numeral = dict(id=wid2,line=1,ordinal=2,text='١',characters=[dict(text='١',position='unknown')])
    transcript = dict(pageId=PID,words=[word,numeral])
    if with_long_run:
        wid3 = PID+':l1-w3'
        chars = 'ولعلعل'
        forms = ['isolated','initial','medial','medial','medial','final']
        transcript['words'].append(dict(id=wid3,line=1,ordinal=3,text=chars,characters=[
            dict(text=c,position=f) for c,f in zip(chars,forms)]))
    ts = encoded(dict(producer='medinamushafanalyzer-QuranTextByWord-v1',pages=[transcript])).encode()
    (root/'inputs/transcript.json').write_bytes(ts)
    # Page-raster offsets on an 8x8 grid. The right-hand body includes two connected letters.
    paths = [dict(id='l1-w1-p0',bbox=[1,1,4,2],runs=[9,4,17,4]),
             dict(id='l1-w1-p1',bbox=[1,4,1,1],runs=[33,1]),
             dict(id='l1-w1-p2',bbox=[2,4,1,1],runs=[34,1]),
             dict(id='l1-w1-p3',bbox=[2,0,1,1],runs=[2,1]),
             dict(id='l1-w1-p4',bbox=[0,1,1,2],runs=[8,1,16,1]),
             dict(id='l1-w2-p0',bbox=[5,3,1,1],runs=[29,1])]
    groups=[]; subs=[]; validation=[]
    starts=[(S1,paths[0:4],[(0,'بِ',[(0,'single-dot',1),(1,'kasra',2)]),(2,'رَ',[(3,'fatha',3)])]),
            (S2,paths[4:5],[(4,'أ',[(4,'hamza-above',None)])])]
    for sid,members,assignments in starts:
        owners=[];results=[]
        for offset,chunk,links in assignments:
            oid=WID+':c'+str(offset)
            slots=[];inks=[];predictions=[];missing=[]
            for target,expected,path in links:
                ch=text[target];cid=WID+':c'+str(target)
                slots.append(dict(characterOffset=target,sourceCharacter=ch,codePoint=f'U+{ord(ch):04X}',type=expected))
                if path is None:
                    missing.append(target);continue
                p=paths[path];gid=oid+':g'+str(len(inks)+1)
                g=dict(id=gid,runs=p['runs'],bbox=p['bbox'],pathIds=[p['id']],transcriptLink=dict(
                    status='linked',baseOffset=offset,baseCharacter=text[offset],characterId=cid,
                    characterOffset=target,expectedType=expected,method='native-geometry'))
                inks.append(g);groups.append(g)
                raw='kasra' if expected=='fatha' else expected
                predictions.append(dict(id=gid,transcriptCharacterId=cid,type=raw,inventoryType=raw,kind='mark',
                                        characterMatch='match' if expected==raw else 'different',margin=.95,score=.98))
            owners.append(dict(id=oid,text=chunk,character=text[offset],ordinal=1+offset//2,offset=offset,role='letter',
                               expectedSlots=slots,groups=inks))
            results.append(dict(id=oid,predictions=predictions,charactersWithoutInk=missing))
        subs.append(dict(id=sid,wordId=WID,text=''.join(o['text'] for o in owners),owners=owners,
                         bodyPathId=members[0]['id'],pathIds=[p['id'] for p in members]))
        validation.append(dict(id=sid,owners=results))
    doc=dict(pageId=PID,schemaVersion=5,raster=dict(width=8,height=8),assignmentUnchangedAfterValidation=True,
             nativeAssignment=dict(producer='medinamushafanalyzer-native-marks-v1',transcriptSha256=sha(encoded(transcript).encode())),
             provenance=dict(sourceFormat='ai-pdf',sourceSha256=source_sha,pdfSha256=source_sha,pdfToRaster=[1,0,0,1,0,0],transcriptSnapshotSha256=sha(ts)),
             classifier=dict(modelFiles={}),policy=dict(classifierUsedForAssignment=False),assignmentSha256='assignment',
             conservation=dict(everyPathAccounted=True,inkSha256='ink'),paths=paths,groups=groups,subwords=subs,
             words=[dict(id=WID,text=text,pathIds=[p['id'] for p in paths[:-1]]),dict(id=wid2,text='١',pathIds=[paths[-1]['id']])],
             ignoredSignGroups=[dict(id=wid2+'-s1',text='١',pathIds=[paths[-1]['id']])],validation=validation)
    summary=dict(classifiedGroups=3,letters=3,subwords=2,signOnlyGroups=0,characterLinkMismatches=1,
                 charactersWithoutInk=1,ambiguousInkGroups=0,unmatchedInkGroups=0)
    if with_long_run:
        ids = []
        for sub, offsets in enumerate(([0],range(1,6)),1):
            sid,pid=wid3+'-s'+str(sub),'l1-w3-p'+str(sub)
            ids.append(pid)
            doc['paths'].append(dict(id=pid,bbox=[sub,6,1,1],runs=[48+sub,1]))
            owners=[dict(id=wid3+':c'+str(i),text=chars[i],character=chars[i],ordinal=i+1,offset=i,
                         role='letter',expectedSlots=[],groups=[]) for i in offsets]
            doc['subwords'].append(dict(id=sid,wordId=wid3,text=''.join(o['text'] for o in owners),
                                        owners=owners,bodyPathId=pid,pathIds=[pid]))
            doc['validation'].append(dict(id=sid,owners=[dict(id=o['id'],predictions=[],charactersWithoutInk=[]) for o in owners]))
        doc['words'].append(dict(id=wid3,text=chars,pathIds=ids))
        summary.update(letters=9,subwords=4)
    buf=encoded(doc).encode()
    (root/(PID+'.json')).write_bytes(buf)
    (root/'manifest.json').write_text(encoded(dict(schemaVersion=5,failures=[],requestedPageIds=[PID],summary=summary,
             pages=[dict(pageId=PID,reportSha256=sha(buf),assignmentSha256='assignment',summary=summary)])))


class FreshReferenceTests(unittest.TestCase):
    def test_uses_selected_reference_and_still_detects_changed_totals(self):
        for n in (0,11,79):
            self.assertEqual(difference_count_errors(n,n,n,n),[])
        self.assertTrue(difference_count_errors(11,12,11,12))
        self.assertTrue(difference_count_errors(10,11,11,11))
        self.assertTrue(difference_count_errors(11,10,11,11))
        self.assertTrue(difference_count_errors(11,11,11,11,expected=79))


class CatalogTests(unittest.TestCase):
    def setUp(self):
        tmp=TemporaryDirectory();self.addCleanup(tmp.cleanup)
        self.root=Path(tmp.name);self.report=self.root/'report';self.file=self.root/'corpus.sqlite'
        write_fixture(self.report);build(self.file,self.report)
        self.db=read(self.file);self.addCleanup(self.db.close)
        self.bundle=dict(version=1,subword=S1,sourceKey=body_source(self.db,S1)['sourceKey'],producer='test-cutter@123',
            parts=[dict(owner=O0,role='body',runs=[11,2,19,2],metrics=dict(advance=3.2,quality='clean',confidence=.95)),
                   dict(owner=O2,role='body',runs=[9,1,17,1],metrics=dict(advance=2.1,kashidaRight=1)),
                   dict(owner=O0,role='connection',nextOwner=O2,runs=[10,1,18,1])])

    def attach(self,*records):
        with closing(sqlite3.connect(self.file)) as db,db:
            db.execute('PRAGMA foreign_keys=ON')
            db.execute('BEGIN IMMEDIATE')
            import_bodies(db,StringIO(''.join(encoded(r)+'\n' for r in records)))

    def test_main_query_matches_consecutive_owners_and_their_native_marks(self):
        run=lambda **p: search(self.db,dict(kind='letters',**p))
        row,=run(q='بِرَ',match='letter',forms='medial,isolated',verdict='different')['rows']
        self.assertEqual((row['sequence_ids'],row['text'],row['group_form'],row['run_start'],row['run_end']),
                         (O0+','+O2,'بِرَ','isolated',0,4))
        # Teh can share the initial tooth with beh; a mark on the following reh
        # still has to be its native fatha even though the classifier calls it kasra.
        self.assertEqual(run(q='تِرَ',match='class')['total'],1)
        for q in ('تِرَ','بَرِ','برِ','رب','رأ'):
            self.assertEqual(run(q=q,match='letter')['total'],0,q)
        self.assertEqual(run(q='بر',within='page003:l1-w1-s2')['total'],0)
        for mark,owner in [('dot',O0),('َ',O2)]:
            match,=search(self.db,dict(kind='marks',q=mark))['rows']
            self.assertEqual(match['owner_id'],owner)
        for bad in ('َب','ب'*9):
            with self.assertRaises(ValueError): run(q=bad)
        with self.assertRaises(KeyError): image(self.db,'letters',O2+','+O0,'crop')

    def test_span_position_is_relative_to_subword_even_midword(self):
        directory=self.root/'with-run';file=self.root/'with-run.sqlite'
        write_fixture(directory,with_long_run=True);build(file,directory)
        with closing(read(file)) as db:
            def rows(q,forms=None):
                return search(db,dict(kind='letters',q=q,forms=forms,match='letter'))['rows']
            self.assertEqual([(r['run_start'],r['group_form']) for r in rows('لعل')],[(1,'initial'),(3,'final')])
            self.assertEqual([(r['run_start'],r['group_form']) for r in rows('علع','isolated,medial')],[(2,'medial')])
            self.assertEqual(rows('لعلعل')[0]['group_form'],'isolated')
            self.assertEqual(rows('ول'),[])  # Adjacent in the word, in different subwords.
            self.assertEqual(rows('لعل','isolated'),[])

    def test_cutter_review_selects_transcript_positions_before_any_cuts(self):
        from cut_review import review_targets

        forms = self.root/'forms.json'
        forms.write_text(encoded(dict(
            initial=[dict(id='first-beh', current='ب')],
            medial=[dict(id='medial-reh', current='ر')],
            final=[dict(id='last-reh', current='ر')],
        )))
        self.assertEqual(review_targets(self.db, [PID], forms, 'first-beh'), {PID: {S1}})
        self.assertEqual(review_targets(self.db, [PID], forms, 'last-reh'), {PID: {S1}})
        self.assertEqual(review_targets(self.db, [PID], forms, 'medial-reh'), {PID: set()})
        with self.assertRaisesRegex(ValueError, 'Unknown template'):
            review_targets(self.db, [PID], forms, 'missing')

    def test_preview_connectors_and_measurements_for_the_run(self):
        self.bundle['parts'][0]['metrics']['kashidaLeft']=1
        self.attach(self.bundle)
        params=dict(kind='letters',sequence='بر',metric='kashida',minConfidence='0.9')
        self.assertEqual(search(self.db,params)['rows'][0]['metric_value'],1)
        self.assertEqual(search(self.db,params,True)['distribution'],[[1,1]])
        connector='M2 1h1'
        self.assertNotIn(connector,image(self.db,'letters',O0,'crop',dark=True))
        self.assertIn(connector,image(self.db,'letters',O0,'crop',left=True,dark=True))
        self.assertIn(connector,image(self.db,'letters',O2,'crop',right=True,dark=True))
        self.assertIn('#99aebf',image(self.db,'letters',O0,'crop',pair=True,dark=True))

    def test_letter_preview_can_omit_surrounding_subword(self):
        pending = image(self.db,'letters',O0,'match',dark=True)
        self.assertNotIn('#5d7387',pending)
        self.assertIn('M2 4h1',pending)
        self.assertIn('#edf4fa',image(self.db,'letters',O0+','+O2,'match',dark=True))
        self.assertIn('viewBox="-4 -4 14 12"',image(self.db,'letters',O0+','+O2,'match',marks=False))
        self.attach(self.bundle)
        focused = image(self.db,'letters',O0,'match',dark=True)
        self.assertIn('viewBox="-4 -4 14 14"',focused)
        self.assertIn('M3 1h2',focused)
        self.assertNotIn('M1 1h1',focused)
        self.assertIn('M1 1h1',image(self.db,'letters',O0,'match',pair=True,dark=True))
        self.assertIn('#5d7387',image(self.db,'letters',O0,'crop',dark=True))
        self.assertIn('#5d7387',image(self.db,'letters',O0,'word',dark=True))
        rows = search(self.db,dict(kind='letters',metric='width',min='1',max='2',sort='largest'))['rows']
        self.assertEqual([(r['id'],r['metric_value']) for r in rows],[(O0,2),(O2,1)])

    def test_whole_word_preview_can_hide_marks_from_the_background(self):
        self.attach(self.bundle)
        with_marks = image(self.db,'letters',O0,'word')
        without_marks = image(self.db,'letters',O0,'word',marks=False)
        self.assertIn('M2 4h1',with_marks)
        self.assertNotIn('M2 4h1',without_marks)
        self.assertIn('M3 1h2',without_marks)
        self.assertIn('viewBox="-5 -4 15 12"',without_marks)
        self.assertIn('viewBox="-4 -4 14 12"',image(self.db,'subwords',S1,'crop',marks=False))
        other_subword = image(self.db,'subwords',WID+'-s2','crop',marks=False)
        self.assertIn(other_subword.split(' d="')[1].split('"')[0],without_marks)

    def test_unbounded_kashida_keeps_zero_and_unmeasured_matches(self):
        self.bundle['parts'][0]['metrics']['kashidaLeft'] = 0
        self.bundle['parts'][1]['metrics']['kashidaRight'] = 0
        self.attach(self.bundle)
        query = dict(kind='letters', sort='largest')
        total = search(self.db,query)['total']
        for metric in ('kashidaLeft','kashidaRight','kashida'):
            params = dict(query, metric=metric)
            self.assertEqual(search(self.db,params)['total'],total)
        params = dict(query, metric='kashidaRight')
        rows = search(self.db,params)['rows']
        self.assertEqual([r['metric_value'] for r in rows],[0,None,None])
        for sort in ('largest','smallest'):
            for offset,row in enumerate(rows):
                self.assertEqual(search(self.db,dict(params,sort=sort,limit='1',offset=str(offset)))['rows'],[row])
        stats = search(self.db,params,True)
        self.assertEqual((stats['distribution'],stats['unmeasured']),([[0,1]],2))
        self.assertEqual(search(self.db,dict(params,min='0'))['total'],1)
        self.assertEqual(search(self.db,dict(params,max='0'))['total'],1)
        self.assertEqual(search(self.db,dict(params,min='1'))['total'],0)

    def test_zero_coverage_still_has_text_and_mark_links(self):
        self.assertEqual(verify(self.db)['pending'],2)
        self.assertEqual(search(self.db,dict(kind='letters',coverage='pending'))['total'],3)
        self.assertEqual(search(self.db,dict(kind='words',coverage='sign'))['total'],1)
        self.assertEqual(search(self.db,dict(kind='letters',sequence='بِر',form='isolated'))['total'],1)
        self.assertEqual(search(self.db,dict(kind='letters',sequence='برأ'))['total'],0)
        row,=search(self.db,dict(kind='marks',character=WID+':c1'))['rows']
        self.assertEqual((row['owner_id'],row['codepoint'],row['subword_id']),(O0,'U+0650',S1))
        self.assertEqual(search(self.db,dict(kind='marks',q='page003:l1-w1-p2'))['total'],1)
        self.assertEqual(search(self.db,dict(kind='marks',verdict='issues'))['total'],2)
        self.assertEqual(search(self.db,dict(kind='parts'))['total'],0)
        stats=search(self.db,dict(kind='letters'),True)
        self.assertEqual((stats['unmeasured'],stats['distribution']),(3,[]))
        self.assertIn('M2 4h1',image(self.db,'letters',O0,'crop'))
        self.assertIn('M5 3h1',image(self.db,'words',PID+':l1-w2','word'))

    def test_partial_replay_and_clean_database_produce_identical_ink(self):
        before=search(self.db,dict(kind='marks'))['rows']
        self.attach(self.bundle);self.attach(self.bundle)
        changed=search(self.db,dict(kind='marks'))['rows']
        self.assertEqual([{k:v for k,v in r.items() if k!='coverage'} for r in before],
                         [{k:v for k,v in r.items() if k!='coverage'} for r in changed])
        self.assertEqual(search(self.db,dict(kind='words',coverage='partial'))['total'],1)
        self.assertEqual(search(self.db,dict(kind='letters',coverage='segmented'))['total'],2)
        self.assertEqual(search(self.db,dict(kind='parts',role='connection'))['rows'][0]['next_owner_id'],O2)
        row,=search(self.db,dict(kind='letters',sequence='بر',metric='advance',sort='largest'))['rows']
        self.assertAlmostEqual(row['metric_value'],5.3)
        self.assertEqual(detail(self.db,WID)['subwords'][1]['coverage'],'pending')
        build(self.file,self.report)
        rebuilt=self.root/'rebuilt.sqlite'
        replay_existing_index(self.file,rebuilt)
        with closing(read(rebuilt)) as other:
            self.assertEqual(verify(other,True),verify(self.db,True))
            a,b=StringIO(),StringIO();export_bodies(self.db,a);export_bodies(other,b)
            self.assertEqual(a.getvalue(),b.getvalue())
            self.assertEqual(image(other,'parts',O0+'/connection','crop'), image(self.db,'parts',O0+'/connection','crop'))
            for table in ('native_marks','catalog_subwords','catalog_owners','catalog_parts','catalog_measurements'):
                sql='SELECT * FROM '+table+' ORDER BY 1,2'
                self.assertEqual([tuple(r) for r in self.db.execute(sql)],[tuple(r) for r in other.execute(sql)])

    def test_invalid_bundle_rolls_back_all_subwords(self):
        bad=deepcopy(self.bundle);bad['subword']=S2;bad['sourceKey']=body_source(self.db,S2)['sourceKey']
        with self.assertRaisesRegex(ValueError,'invalid part owner'):
            self.attach(self.bundle,bad)
        self.assertEqual(verify(self.db)['segmented'],0)
        self.attach(self.bundle)
        for change,message in [('key','source key'),('overlap','overlap'),('gap','omit'),('letter','each transcript'),('neighbor','adjacent')]:
            with self.subTest(change=change):
                bad=deepcopy(self.bundle)
                if change=='key':bad['sourceKey']='stale'
                elif change=='overlap':bad['parts'][-1]['runs']=[10,2,18,1]
                elif change=='gap':bad['parts'][-1]['runs']=[10,1]
                elif change=='letter':bad['parts'].pop(1)
                else:bad['parts'][-1]['nextOwner']=O0
                with self.assertRaisesRegex(ValueError,message):self.attach(bad)
                self.assertEqual(verify(self.db)['parts'],3)

    def test_all_subwords_and_corrupt_report(self):
        self.attach(self.bundle,dict(version=1,subword=S2,sourceKey=body_source(self.db,S2)['sourceKey'],producer='test-cutter@123',
                                    parts=[dict(owner=WID+':c4',role='body',runs=[8,1,16,1])]))
        self.assertEqual(search(self.db,dict(kind='words',coverage='segmented'))['total'],1)
        self.assertEqual(verify(self.db,True)['pending'],0)
        p=self.report/(PID+'.json');p.write_bytes(p.read_bytes()+b' ')
        with self.assertRaisesRegex(ValueError,'hash mismatch'):build(self.file,self.report)
        self.assertEqual(verify(self.db,True)['segmented'],2)

    def test_updated_native_body_cannot_silently_reuse_an_old_cut(self):
        self.attach(self.bundle)
        report_file = self.report/(PID+'.json')
        manifest_file = self.report/'manifest.json'
        doc = json.loads(report_file.read_text())
        doc['paths'][0]['runs'] = [10,3,17,4]
        changed = encoded(doc).encode()
        report_file.write_bytes(changed)
        manifest = json.loads(manifest_file.read_text())
        manifest['pages'][0]['reportSha256'] = sha(changed)
        manifest_file.write_text(encoded(manifest))
        with self.assertRaisesRegex(ValueError,'source key'):
            build(self.file,self.report)
        self.assertEqual(body_source(self.db,S1)['runs'],[9,4,17,4])
        self.assertEqual(verify(self.db,True)['segmented'],1)
