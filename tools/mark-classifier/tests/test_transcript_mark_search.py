import hashlib
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest
from contextlib import closing

from automatic.common import unpack_runs
from automatic.transcript_marks import import_report, replay_existing_index
from automatic.transcript_mark_search import image, metadata, report_file, search


def page(pid, fused=False):
    wid = pid+':l1-w1'
    base, intrinsic = ('أ','hamza-above') if fused else ('ب','single-dot')
    vowel, vowel_type = ('َ','fatha') if fused else ('ِ','kasra')
    text = base+vowel
    oid = wid+':c0'
    slots = [dict(characterOffset=i, sourceCharacter=c, codePoint=f'U+{ord(c):04X}', type=t)
             for i,(c,t) in enumerate(((base,intrinsic),(vowel,vowel_type)))]
    groups, predictions = [], []
    paths = [dict(id='l1-w1-p0', bbox=[0,0,2,2], runs=[0,2,8,2])]
    for i in ([1] if fused else [1,0]):
        slot = slots[i]
        gid, cid = oid+f':g{len(groups)+1}',wid+f':c{i}'
        path = dict(id='l1-w1-p'+str(i+1), bbox=[i,3,1,1], runs=[24+i,1])
        paths.append(path)
        groups.append(dict(id=gid,runs=path['runs'],bbox=path['bbox'],pathIds=[path['id']],
                           transcriptLink=dict(status='linked',characterId=cid,characterOffset=i,
                               expectedType=slot['type'],method='native-geometry',baseCharacter=base,baseOffset=0)))
        raw = 'fatha' if i==1 and not fused else slot['type']
        predictions.append(dict(id=gid,characterMatch='match' if raw==slot['type'] else 'different',
                                transcriptCharacterId=cid,inventoryType=raw,type=raw,kind='mark',score=.9,margin=.8))
    sid = wid+'-s1'
    owner = dict(id=oid,role='letter',character=base,offset=0,expectedSlots=slots,groups=groups)
    return dict(pageId=pid,schemaVersion=5,raster=dict(width=8,height=8),provenance=dict(sourceSha256='svg',pdfSha256='pdf'),
                assignmentSha256='assignment',assignmentUnchangedAfterValidation=True,policy=dict(classifierUsedForAssignment=False),
                conservation=dict(everyPathAccounted=True,inkSha256='ink'),nativeAssignment={},classifier=dict(modelFiles={}),
                paths=paths,words=[dict(id=wid,text=text,pathIds=[p['id'] for p in paths])],
                subwords=[dict(id=sid,wordId=wid,owners=[owner])],groups=groups,
                validation=[dict(id=sid,owners=[dict(id=oid,predictions=predictions,charactersWithoutInk=[0] if fused else [])])])


class TranscriptMarkSearchTests(unittest.TestCase):
    def setUp(self):
        tmp=tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root=Path(tmp.name)
        self.database=self.root/'corpus.sqlite'
        self.report=self.root/'report'
        self.report.mkdir()
        self.docs=[page('page001'),page('page002',fused=True)]
        self.make_vector(self.database)
        entries=[]
        for d in self.docs:
            pid=d['pageId']
            data=json.dumps(d,ensure_ascii=False).encode()
            (self.report/(pid+'.json')).write_bytes(data)
            entries.append(dict(pageId=pid,reportSha256=hashlib.sha256(data).hexdigest(),assignmentSha256=d['assignmentSha256'],
                summary=dict(classifiedGroups=len(d['groups']),characterLinkMismatches=int(pid=='page001'),
                             charactersWithoutInk=int(pid=='page002'),ambiguousInkGroups=0,unmatchedInkGroups=0)))
        (self.report/'manifest.json').write_text(json.dumps(dict(schemaVersion=5,pages=entries,failures=[],
                                               requestedPageIds=[d['pageId'] for d in self.docs])))
        import_report(self.database,self.report)

    def make_vector(self, path):
        with closing(sqlite3.connect(path)) as db, db:
            db.executescript('''CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT);
                CREATE TABLE pages(page_id TEXT PRIMARY KEY,source_sha TEXT,pdf_sha TEXT,width INTEGER,height INTEGER);
                CREATE TABLE words(id TEXT PRIMARY KEY,page_id TEXT,text TEXT,bases TEXT);
                CREATE TABLE items(id TEXT PRIMARY KEY,human_reviewed INTEGER); INSERT INTO items VALUES ('human',1);''')
            for d in self.docs:
                pid=d['pageId'];word=d['words'][0]
                db.execute('INSERT INTO pages VALUES (?,\'svg\',\'pdf\',8,8)',(pid,))
                db.execute('INSERT INTO words VALUES (?,?,?,?)',(word['id'],pid,word['text'],word['text'][0]))

    def test_correct_character_and_base_even_when_classifier_disagrees(self):
        with closing(sqlite3.connect(self.database)) as db:
            db.row_factory=sqlite3.Row
            self.assertEqual(metadata(db)['masks'],3)
            self.assertEqual(search(db,{'verdict':'issues'})['total'],2)
            row, = search(db,{'q':'page001:l1-w1:c1','expected':'kasra','predicted':'fatha','base':'ب'})['rows']
            self.assertEqual((row['word_offset'],row['base_offset'],row['source_character'],row['verdict']), (1,0,'ِ','different'))
            self.assertEqual(search(db,{'page':'1','q':'l1-w1-p2'})['rows'][0]['id'],row['id'])
            self.assertEqual(search(db,{'q':'U+0650'})['rows'][0]['id'],row['id'])
            self.assertEqual(search(db,{'q':'kasra'})['rows'][0]['id'],row['id'])
            self.assertEqual(unpack_runs(db.execute('SELECT runs FROM native_marks WHERE id=?',(row['id'],)).fetchone()[0]),[25,1])
            self.assertIn('M1 3h1',image(db,row['id'],False))
            self.assertIn('#b5beb7',image(db,row['id'],True))
            missing,=search(db,{'q':'page002:l1-w1:c0'})['rows']
            self.assertEqual((missing['verdict'],missing['classifier_type'],missing['expected_type']),('missing',None,'hamza-above'))
            self.assertEqual(search(db,{'verdict':'missing'})['total'],1)
            with self.assertRaises(ValueError):
                report_file(db,'../manifest.json')

    def test_lookup_by_source_path_finds_its_split_components(self):
        with closing(sqlite3.connect(self.database)) as db, db:
            db.row_factory=sqlite3.Row
            db.execute("UPDATE native_marks SET path_ids=? WHERE id=?", (json.dumps(['l1-w1-p2-stroke-1']),'page001:l1-w1:c0:g1'))
            db.execute("UPDATE native_marks SET path_ids=? WHERE id=?", (json.dumps(['l1-w1-p20']),'page001:l1-w1:c0:g2'))
            for q in ('page001:l1-w1-p2','page001:l1-w1-p2-stroke-1'):
                self.assertEqual([r['id'] for r in search(db,{'q':q})['rows']], ['page001:l1-w1:c0:g1'])

    def test_reimport_and_rebuild_preserve_rows_and_human_reviews(self):
        rebuilt=self.root/'rebuilt.sqlite'
        self.make_vector(rebuilt)
        replay_existing_index(self.database,rebuilt)
        import_report(self.database,self.report)
        with closing(sqlite3.connect(self.database)) as db, closing(sqlite3.connect(rebuilt)) as other:
            for table in ('native_marks','native_mark_words','native_mark_pages','meta','items'):
                self.assertEqual(db.execute('SELECT * FROM '+table).fetchall(),other.execute('SELECT * FROM '+table).fetchall())
            self.assertEqual(db.execute('SELECT * FROM items').fetchall(),[('human',1)])
            self.assertEqual(db.execute('PRAGMA foreign_key_check').fetchall(),[])

    def test_late_hash_failure_rolls_back_and_different_artwork_is_rejected(self):
        last=self.report/'page002.json'
        saved=last.read_bytes(); last.write_bytes(saved+b' ')
        with self.assertRaisesRegex(ValueError,'page002: page report hash mismatch'):
            import_report(self.database,self.report)
        last.write_bytes(saved)
        with closing(sqlite3.connect(self.database)) as db, db:
            self.assertEqual(db.execute('SELECT count(*) FROM native_marks').fetchone()[0],4)
            db.execute("UPDATE pages SET pdf_sha='different' WHERE page_id='page002'")
        with self.assertRaisesRegex(ValueError,'page002: SVG, PDF or raster differs'):
            import_report(self.database,self.report)
        with closing(sqlite3.connect(self.database)) as db:
            self.assertEqual(db.execute('SELECT count(*) FROM native_mark_pages').fetchone()[0],2)
