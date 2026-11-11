from collections import Counter
from contextlib import closing
from io import StringIO
import json
from pathlib import Path
import sqlite3
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
import zlib
from xml.etree import ElementTree as ET

from automatic.transcript_mark_search import image as mark_preview
from compact import compact_catalog
from page_join import choose_boundary, outline_edges
from page_view import page_svg
from segment_letters import NATIVE, log_page, publish_page, record_kashida_spans, segment, selected_pages
from rebuild_check import compare_rebuild
from search import body_source, image, search
from store import attach_record, build, pack_runs, read, sha
from test_catalog import O0, O2, PID, S1, S2, write_fixture

SVG = '{http://www.w3.org/2000/svg}'
OUTLINE = dict(d='M1 1H7V7H1Z', fillRule='nonzero')
PAGE_ART = dict(frame=0, opening=dict(OUTLINE,bounds=[1,1,6,6]), paint=[OUTLINE], headings=[], ayahs=[])


def pack_vectors(vectors=None):
    data = dict(paths={}, parts={}, rasterMarks=[], pageArt=PAGE_ART)
    data.update(vectors or {})
    return zlib.compress(json.dumps(data).encode())


class CutterLogTests(unittest.TestCase):
    def test_partial_and_unmatched_cuts_have_distinct_diagnostics(self):
        log = StringIO()
        missing = dict(position='medial',character='ص',status='no-match',candidates=['sad-medi'])
        matched = dict(position='initial',character='ل',status='matched')
        results = {
            'partial': dict(status='incomplete',text='لصح',letters=[matched,missing],trace=['Contour rejected']),
            'unmatched': dict(status='incomplete',text='صح',letters=[missing]),
            'accepted': dict(status='template'),
        }
        self.assertEqual(log_page(log,'page435',results),
                         {'partial-match':1,'no-match':1,'template':1})
        self.assertIn('partial-match: partial لصح',log.getvalue())
        self.assertIn('no-match: unmatched صح',log.getvalue())
        self.assertIn('medial ص: no-match (sad-medi)',log.getvalue())
        self.assertIn('Contour rejected',log.getvalue())

    def test_aborted_segmentation_publishes_its_log(self):
        with TemporaryDirectory() as folder:
            database, log = (Path(folder)/name for name in ('catalog.sqlite','cuts.log'))
            problem = 'invalid native segmentation'
            with patch('segment_letters.segment_pages',side_effect=ValueError(problem)):
                with self.assertRaisesRegex(ValueError,problem):
                    segment(database,'435',1,log_path=log)
            self.assertIn('Pages: 435',log.read_text())
            self.assertIn('Catalog update aborted: '+problem,log.read_text())


class ConnectorTests(unittest.TestCase):
    def test_lengths_follow_cut_midpoints_and_writing_order(self):
        owners = ('first', 'middle', 'last')
        bodies = [dict(role='body', owner=owner, metrics={}) for owner in owners]
        connector = dict(role='kashida', owner='first', nextOwner='middle',
                         caps=[[[20, 3], [18, 7]], [[9, 0], [10, 6]]])
        # Native body order can differ from transcript order around a tatweel.
        record_kashida_spans([connector, *reversed(bodies)], owners)
        self.assertEqual([part['metrics'] for part in bodies],
                         [dict(kashidaLeft=9.5),dict(kashidaRight=9.5,kashidaLeft=0),dict(kashidaRight=0)])

    def boundary(self, widths, turn=lambda x,y:(x,y)):
        # A symmetric ribbon viewed from above. Its caps touch the two bodies.
        upper = [turn(x,w/2) for x,w in widths]
        lower = [turn(x,-w/2) for x,w in widths]
        polygon = upper + lower[::-1]
        return choose_boundary([[upper[0],lower[0]],[upper[-1],lower[-1]]],
                               list(zip(polygon,polygon[1:]+polygon[:1])))

    def test_off_center_neck_does_not_shift_the_cut(self):
        widths = [(0,4),(3,1),(10,5)]
        for transform in (lambda x,y:(x,y),lambda x,y:(-y,10-x)):
            point,axis,method = self.boundary(widths,transform)
            target = transform(5,0)
            self.assertEqual(method,'midpoint')
            self.assertAlmostEqual(point[0],target[0],delta=.16)
            self.assertAlmostEqual(point[1],target[1],delta=.16)
            endpoint = transform(10,0)
            self.assertGreater(sum((e-p)*n for e,p,n in zip(endpoint,point,axis)),0)

    def test_uniform_or_tapered_stroke_uses_middle(self):
        for widths in ([(0,3),(10,3)],[(0,5),(10,2)]):
            self.assertEqual(self.boundary(widths),((5,0),(1,0),'midpoint'))

    def test_midpoint_uses_horizontal_ink_extent_and_local_normal(self):
        caps = [[(0,0),(2,2)],[(10,3),(11,5)]]
        polygon = [caps[0][0],caps[1][0],caps[1][1],caps[0][1]]
        point,axis,method = choose_boundary(caps,list(zip(polygon,polygon[1:]+polygon[:1])))
        self.assertEqual(method,'midpoint')
        self.assertEqual(point[0],5.5)
        self.assertAlmostEqual(point[1],(5.5*.3+2+3.5/3)/2)
        # The clipping axis runs along the stroke, perpendicular to the cut.
        self.assertAlmostEqual(axis[1]/axis[0],(.3+1/3)/2)

    def test_curved_yeh_reh_stroke_uses_middle(self):
        # Page 4, line 8: the cut follows the local curve at the horizontal midpoint.
        path = ('M20.706 .911C18.986 3.111 8.554 6.462 .924 5.168L.924 12.326'
                'C7.276 12.992 17.266 10.763 20.546 7.797C20.600 7.751 20.654 7.704 20.706 7.656')
        caps = [[(20.706,.911),(20.706,7.656)],[(.924,5.168),(.924,12.326)]]
        (x,y),axis,method = choose_boundary(caps,outline_edges(path))
        self.assertEqual(method,'midpoint')
        self.assertAlmostEqual(x,10.815)
        # Values from intersecting the original cubics at x=10.815.
        self.assertAlmostEqual(y,8.24579,delta=.01)
        self.assertAlmostEqual(axis[1]/axis[0],-.19519,delta=.01)


class PageViewTests(unittest.TestCase):
    def setUp(self):
        temporary = TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        folder = Path(temporary.name)
        self.database = folder/'pilot.sqlite'
        write_fixture(folder/'report')
        build(self.database,folder/'report')
        self.db = sqlite3.connect(self.database)
        self.addCleanup(self.db.close)
        self.db.execute('PRAGMA foreign_keys=ON')
        self.key = self.db.execute('SELECT body_key FROM catalog_subwords WHERE id=?',(S1,)).fetchone()[0]
        self.report,self.source = self.db.execute('SELECT report_sha,source_sha FROM catalog_pages JOIN pages USING(page_id)').fetchone()
        self.parts = [dict(owner=O0,role='body',runs=[11,2,19,2]),dict(owner=O2,role='body',runs=[9,1,17,1]),
                      dict(owner=O0,role='kashida',nextOwner=O2,runs=[10,1,18,1])]

    def batch(self,status):
        return dict(report=self.report,source=self.source,keys={S1:self.key},
                    results={S1:dict(status=status,parts=self.parts)})

    def test_compact_catalog_replays_vectors_and_loads_body_ink_only_on_demand(self):
        directory = self.database.parent
        report = directory/'vector-report'
        database = directory/'vector.sqlite'
        write_fixture(report)
        build(database,report)
        shape = dict(d='M1 1L3 1L3 3L1 3L1 1')
        with closing(read(database)) as db:
            body = body_source(db,S2)
            mark_ids = [(r[0],json.loads(r[1])) for r in db.execute(
                'SELECT id,path_ids FROM native_marks WHERE runs IS NOT NULL')]
            report_sha = db.execute('SELECT report_sha FROM catalog_pages').fetchone()[0]
            key = body_source(db,S1)['sourceKey']
        single = dict(owner=body['owners'][0]['id'],role='body',runs=body['runs'])
        vectors = dict(paths={name:shape for _,ids in mark_ids for name in ids},parts={
            part['owner']+'/'+part['role']: dict(shape,ink=sha(pack_runs(part['runs'])))
            for part in [*self.parts,single]
        },rasterMarks=[])
        vectors['parts'][O0+'/kashida']['caps'] = [[[1,1],[1,3]],[[3,1],[3,3]]]
        batch = dict(report=report_sha,source=self.source,keys={S1:key,S2:body['sourceKey']},
                     results={S1:dict(status='template',parts=self.parts),S2:dict(status='single-letter',parts=[single])})
        packed = pack_vectors(vectors)
        with closing(sqlite3.connect(database)) as db, db:
            publish_page(db,PID,batch,packed,NATIVE+'fixture',Counter())
        with closing(read(database)) as db:
            before = page_svg(db,'3',colored=True)
        compact_catalog(database)
        with closing(sqlite3.connect(database)) as db, db:
            db.execute('PRAGMA foreign_keys=ON')
            publish_page(db,PID,batch,packed,NATIVE+'fixture',Counter())
        with closing(read(database)) as db:
            self.assertEqual(page_svg(db,'3',colored=True),before)
            self.assertEqual(body_source(db,S2),body)
            self.assertIn(shape['d'],mark_preview(db,mark_ids[0][0],True))
        self.assertTrue(compare_rebuild(database)['passed'])

        pending = dict(batch, results={**batch['results'], S1:dict(status='incomplete')})
        pending_vectors = dict(vectors, parts={single['owner']+'/body':vectors['parts'][single['owner']+'/body']})
        counts = Counter()
        with closing(sqlite3.connect(database)) as db, db:
            db.execute('PRAGMA foreign_keys=ON')
            publish_page(db,PID,pending,pack_vectors(pending_vectors),NATIVE+'fixture',counts)
        with closing(read(database)) as db:
            self.assertEqual(db.execute('SELECT count(*) FROM catalog_splits WHERE subword_id=?',(S1,)).fetchone()[0],0)
            page = ET.fromstring(page_svg(db,'3',colored=True))
        self.assertEqual(counts, {'pending/incomplete':1,'single-letter':1})
        self.assertEqual(page.find('.//'+SVG+f'g[@data-owner="{O0}"]').get('data-quality'),'pending')
        original = page.find(SVG+'g[@mask="url(#paper)"]')
        self.assertIsNotNone(original.find(SVG+'path'))
        self.assertTrue(compare_rebuild(database)['passed'])
        invalid = dict(pending, results={**pending['results'], S1:dict(status='uncovered-ink')})
        with closing(sqlite3.connect(database)) as db, db:
            with self.assertRaisesRegex(ValueError,'uncovered-ink'):
                publish_page(db,PID,invalid,pack_vectors(pending_vectors),NATIVE+'fixture',Counter())

    def test_search_previews_use_source_curves_and_matching_cuts(self):
        curve = 'M1 1C1 2 2 3 3 1Z'
        cut = 'M3 1C4 2 4 3 3 3Z'
        outline = dict(d=curve,fillRule='evenodd')
        mark, ids = self.db.execute(
            'SELECT id,path_ids FROM native_marks WHERE owner_id=? AND runs IS NOT NULL ORDER BY seq LIMIT 1',
            (O0,)).fetchone()
        mark_path, = json.loads(ids)
        vectors = dict(paths={'l1-w1-p0':outline,mark_path:outline},parts={
            O0+'/body':dict(d=cut,ink=sha(pack_runs(self.parts[0]['runs']))),
            O2+'/body':dict(d='M1 1C1 2 2 3 1 3Z',ink='stale'),
        })
        with self.db:
            publish_page(self.db,PID,self.batch('template'),
                         pack_vectors(vectors),NATIVE+'fixture',Counter())
        with closing(read(self.database)) as reader:
            for kind, identifier, context in [('words',S1.rsplit('-s',1)[0],'crop'),
                                               ('marks',mark,'crop'),('letters',O0,'word')]:
                self.assertIn(f'fill-rule="evenodd" d="{curve}"',image(reader,kind,identifier,context))
            self.assertIn(cut,image(reader,'parts',O0+'/body','crop'))
            stale_preview = image(reader,'parts',O2+'/body','crop')
            self.assertNotIn(vectors['parts'][O2+'/body']['d'],stale_preview)
            self.assertIn('M1 1h1v1h-1z',stale_preview)

    def test_letter_sequence_preview_distinguishes_bodies_and_connector(self):
        paths = ['M3 1L5 1L5 3L3 3L3 1', 'M1 1L2 1L2 3L1 3L1 1', 'M2 1L3 1L3 3L2 3L2 1']
        vectors = {'parts': {
            part['owner']+'/'+part['role']: dict(d=path, ink=sha(pack_runs(part['runs'])))
            for part,path in zip(self.parts,paths)
        }}
        with self.db:
            publish_page(self.db,PID,self.batch('template'),pack_vectors(vectors),NATIVE+'fixture',Counter())
        with closing(read(self.database)) as reader:
            row, = search(reader,dict(kind='letters',sequence='بر',match='letter'))['rows']
            for context in ('match','word'):
                svg = ET.fromstring(image(reader,'letters',row['sequence_ids'],context))
                fills = {p.get('d'):p.get('fill') for p in svg.findall(SVG+'path')}
                self.assertEqual(len({fills[path] for path in paths}),3)
            single = ET.fromstring(image(reader,'letters',O2,'match'))
            self.assertNotEqual(single.find(SVG+'path').get('fill'),fills[paths[1]])

    def test_native_connector_reaches_the_longest_search_on_both_letters(self):
        query = dict(kind='letters',metric='kashidaLeft',sort='largest',min='0')
        with closing(read(self.database)) as reader:
            self.assertIn('no kashida lengths yet', search(reader,query)['emptyMessage'])
        for part in self.parts[:2]:
            part['metrics'] = dict(quality='template')
        self.parts[-1]['caps'] = [[[3,1],[3,3]], [[1.5,1],[1.5,3]]]
        record_kashida_spans(self.parts, [O0,O2])
        with self.db:
            publish_page(self.db,PID,self.batch('template'),pack_vectors(),NATIVE+'fixture',Counter())
        with closing(read(self.database)) as reader:
            for metric, owner in [('kashidaLeft',O0), ('kashidaRight',O2)]:
                params = dict(query,metric=metric)
                row, = search(reader,params)['rows']
                self.assertEqual((row['id'],row['metric_value']),(owner,1.5))
                self.assertEqual(search(reader,params,True)['distribution'],[[1.5,1]])
            self.assertEqual(search(reader,dict(query,min='10'))['emptyMessage'],'No measured kashidas match these filters.')

    def test_replaces_automatic_cuts_and_preserves_reviewed(self):
        counts = Counter()
        producer = NATIVE+'fixture'
        with self.db:
            publish_page(self.db,PID,self.batch('template'),pack_vectors(),producer,counts)
            publish_page(self.db,PID,self.batch('template'),pack_vectors(),producer,counts)
        self.assertEqual(counts,{'template':2})
        self.assertEqual(self.db.execute('SELECT count(*) FROM catalog_parts').fetchone()[0],3)
        with self.db:
            publish_page(self.db,PID,self.batch('incomplete'),pack_vectors(),producer,counts)
        self.assertEqual(self.db.execute('SELECT count(*) FROM catalog_splits').fetchone()[0],0)
        with self.db:
            attach_record(self.db,dict(subword=S1,version=1,sourceKey=self.key,producer='reviewed',parts=self.parts))
            publish_page(self.db,PID,self.batch('template'),pack_vectors(),producer,counts)
        self.assertEqual(self.db.execute('SELECT producer FROM catalog_splits').fetchone()[0],'reviewed')
        self.assertEqual(counts['preserved'],1)

    def test_broken_partition_rolls_back_earlier_record_and_artwork(self):
        batch = self.batch('template')
        batch['results'][S2] = dict(status='template',parts=self.parts)
        batch['keys'][S2] = 'stale'
        with self.assertRaisesRegex(ValueError,'source key'),self.db:
            publish_page(self.db,PID,batch,pack_vectors(),NATIVE+'fixture',Counter())
        for table in ('catalog_splits','catalog_page_art'):
            self.assertEqual(self.db.execute('SELECT count(*) FROM '+table).fetchone()[0],0)

    def test_subword_update_preserves_art_and_other_outlines(self):
        def packed(parts):
            return pack_vectors(dict(paths={'p': 'source'}, rasterMarks=['mark'], parts=parts))

        def artwork():
            blob, = self.db.execute('SELECT vectors FROM catalog_page_art').fetchone()
            return json.loads(zlib.decompress(blob))

        old = {O0+'/body': 'old', O0+'/kashida': 'old join', 'neighbor/body': 'neighbor'}
        new = {O0+'/body': 'new', O2+'/body': 'new'}
        producer = NATIVE+'fixture'
        with self.db:
            publish_page(self.db,PID,self.batch('template'),packed(old),producer,Counter())
            publish_page(self.db,PID,self.batch('template'),packed(new),producer,Counter(),partial=True)
        expected = dict(paths={'p':'source'}, rasterMarks=['mark'], parts={**new,'neighbor/body':'neighbor'}, pageArt=PAGE_ART)
        self.assertEqual(artwork(), expected)
        with self.db:
            attach_record(self.db,dict(subword=S1,version=1,sourceKey=self.key,producer='reviewed',parts=self.parts))
            publish_page(self.db,PID,self.batch('incomplete'),packed({}),producer,Counter(),partial=True)
        self.assertEqual(artwork(), expected)

    def test_native_page_art_renders_decorations_without_an_embedded_svg(self):
        outline = dict(d='M1 1H7V7H1Z',fillRule='nonzero')
        vectors = dict(paths={},parts={},pageArt=dict(
            frame=0,opening=dict(outline,bounds=[1,1,6,6]),paint=[outline,outline],
            headings=[dict(kind='surah-title',line=1,text='سُورَةُ',paths=[outline]),
                      dict(kind='basmala',line=2,text='بِسْمِ ٱللَّهِ',paths=[outline])],
            ayahs=[dict(kind='ayah',line=3,text='١',paths=[outline],marker=1)]))
        with self.db:
            publish_page(self.db,PID,self.batch('template'),
                         pack_vectors(vectors),NATIVE+'fixture',Counter())
        with closing(read(self.database)) as db:
            page = ET.fromstring(page_svg(db,PID,interior=True))
        self.assertEqual(page.get('viewBox'),'1 1 6 6')
        self.assertIsNone(page.find('.//'+SVG+'image'))
        self.assertEqual([g.get('data-decoration') for g in page.iter(SVG+'g') if g.get('data-decoration')],
                         ['surah-title','basmala','ayah'])

    def test_tatweel_hamza_is_a_separate_letter_in_the_page(self):
        character_id = O2.rsplit(':',1)[0]+':c3'
        shape = dict(d='M1 1L3 1L3 3L1 3Z')
        with self.db:
            self.db.execute("UPDATE words SET text='بِـٔأ' WHERE id=?",(O2.rsplit(':',1)[0],))
            self.db.execute("UPDATE catalog_owners SET character='ـ',text='ـٔ' WHERE id=?",(O2,))
            self.db.execute("""UPDATE native_marks SET base_character='ـ',source_character='ٔ',
                codepoint='U+0654',expected_type='hamza-above' WHERE character_id=?""",(character_id,))
            paths = {name:shape for ids, in self.db.execute('SELECT path_ids FROM native_marks WHERE runs IS NOT NULL')
                     for name in json.loads(ids)}
            publish_page(self.db,PID,self.batch('incomplete'),
                         pack_vectors(dict(paths=paths,parts={})),NATIVE+'fixture',Counter())
        with closing(read(self.database)) as db:
            colored = ET.fromstring(page_svg(db,PID,colored=True))
            monochrome = ET.fromstring(page_svg(db,PID))
        for page in (colored,monochrome):
            word = page.find('.//'+SVG+'g[@class="word"]')
            carrier = word.find(SVG+f'g[@data-owner="{O2}"]')
            hamza = word.find(SVG+f'g[@data-owner="{character_id}"]')
            self.assertEqual(carrier.get('data-text'),'ـ')
            self.assertIsNone(carrier.find(SVG+'use[@data-part="mark"]'))
            self.assertEqual((hamza.get('class'),hamza.get('data-character')),('letter','ٔ'))
            self.assertIsNotNone(hamza.find(SVG+'use[@data-part="body"]'))
            ordinary = word.find(SVG+f'g[@data-owner="{O0}"]')
            self.assertEqual(len(ordinary.findall(SVG+'use[@data-part="mark"]')),2)
            self.assertEqual(hamza.get('fill')==carrier.get('fill'), page is monochrome)

    def test_small_alef_detaches_only_after_fatha_on_the_same_letter(self):
        word_id = O2.rsplit(':',1)[0]
        character_id = word_id+':c4'
        with self.db:
            self.db.execute("UPDATE words SET text='بِرَٰأ' WHERE id=?",(word_id,))
            final_letter = word_id+':c5'
            self.db.execute('UPDATE catalog_owners SET id=?,word_offset=5 WHERE id=?',(final_letter,character_id))
            self.db.execute("UPDATE catalog_owners SET text='رَٰ' WHERE id=?",(O2,))
            self.db.execute('''UPDATE native_marks SET owner_id=?,character_id=?,word_offset=5,base_offset=5
                WHERE character_id=?''',(final_letter,final_letter,character_id))
            self.db.execute("""UPDATE native_marks SET character_id=?,word_offset=4,source_character='ٰ',
                codepoint='U+0670',expected_type='small-alef' WHERE character_id=?""",(character_id,word_id+':c3'))
            vectors = dict(paths={'l1-w1-p3':dict(d='M2 1L3 1L3 2L2 2L2 1')},parts={})
            publish_page(self.db,PID,self.batch('incomplete'),
                         pack_vectors(vectors),NATIVE+'fixture',Counter())
        with closing(read(self.database)) as db:
            page = ET.fromstring(page_svg(db,PID,colored=True))
        alef = page.find('.//'+SVG+f'g[@data-owner="{character_id}"]')
        self.assertEqual(alef.get('data-quality'),'small alef after fatha')
        carrier = page.find('.//'+SVG+f'g[@data-owner="{O2}"]')
        self.assertEqual(carrier.get('data-text'),'رَ')
        self.assertNotEqual(alef.get('fill'),carrier.get('fill'))

        with self.db:
            self.db.execute("UPDATE catalog_owners SET text='رُٰ' WHERE id=?",(O2,))
        with closing(read(self.database)) as db:
            page = ET.fromstring(page_svg(db,PID,colored=True))
        self.assertIsNone(page.find('.//'+SVG+f'g[@data-owner="{character_id}"]'))
        carrier = page.find('.//'+SVG+f'g[@data-owner="{O2}"]')
        self.assertEqual(carrier.get('data-text'),'رُٰ')
        self.assertIsNotNone(carrier.find(SVG+'use[@data-part="mark"]'))

    def test_saved_page_uses_verified_vectors_and_keeps_unresolved_ink_in_source(self):
        shape = dict(d='M1 1L3 1L3 3L1 3L1 1',fillRule='nonzero')
        art = dict(PAGE_ART,paint=[OUTLINE,shape])
        marks = self.db.execute('SELECT id,path_ids FROM native_marks WHERE owner_id=? AND runs IS NOT NULL ORDER BY seq',
                                (O0,)).fetchall()
        vectors = dict(paths={name:shape for _,ids in marks for name in json.loads(ids)}, parts={
            part['owner']+'/'+part['role']: dict(shape,ink=sha(pack_runs(part['runs'])))
            for part in self.parts
        })
        vectors['parts'][O0+'/kashida']['caps'] = [[[1,1],[1,3]],[[3,1],[3,3]]]
        vectors['pageArt'] = art
        with self.db:
            publish_page(self.db,PID,self.batch('template'),
                         pack_vectors(vectors),NATIVE+'fixture',Counter())
        with closing(read(self.database)) as db:
            self.assertEqual(selected_pages(db,'all'),[PID])
            with self.assertRaises(ValueError): selected_pages(db,'3-4')
            page = ET.fromstring(page_svg(db,'3',colored=True))
            modes = {mode: ET.fromstring(page_svg(db,'3',colored=True,kashida=mode))
                     for mode in ('left','right','standalone')}
            cropped = ET.fromstring(page_svg(db,'3',interior=True))
        self.assertEqual(page.get('viewBox'),'0 0 8 8')
        self.assertEqual(cropped.get('viewBox'),'1 1 6 6')
        clipped = cropped.find(SVG+'g[@clip-path="url(#frame-opening)"]')
        self.assertIsNotNone(clipped.find('.//'+SVG+f'g[@data-owner="{O0}"]'))
        background = clipped.find(SVG+'g[@mask="url(#paper)"]')
        self.assertEqual([p.get('d') for p in background.findall(SVG+'path')],[shape['d']])
        groups = {g.get('data-owner'):g for g in page.iter(SVG+'g') if g.get('data-owner')}
        self.assertNotEqual(groups[O0].get('fill'),groups[O2].get('fill'))
        first,second = groups[O0],groups[O2]
        self.assertEqual([p.get('data-part') for p in first if p.tag==SVG+'use'].count('mark'),2)
        a = first.find(SVG+"use[@data-part='join-half']")
        b = second.find(SVG+"use[@data-part='join-half']")
        self.assertEqual(a.get('href'),b.get('href'))
        self.assertNotEqual(a.get('clip-path'),b.get('clip-path'))
        self.assertEqual(a.get('data-split'),'midpoint')
        for mode,svg in modes.items():
            with self.subTest(mode=mode):
                self.assertIsNone(svg.find('.//'+SVG+"use[@data-part='join-half']"))
                connector, = svg.findall('.//'+SVG+"use[@data-part='kashida']")
                self.assertEqual(connector.get('href'),a.get('href'))
                self.assertIsNone(connector.get('clip-path'))
                parent = next(g for g in svg.iter(SVG+'g') if connector in g)
                if mode=='standalone':
                    self.assertEqual(parent.get('class'),'kashida')
                else:
                    self.assertEqual(parent.get('data-owner'),O2 if mode=='left' else O0)
        self.assertEqual(page.get('data-pending'),'1')
        self.assertEqual(page.get('data-pending-marks'),'1')
        self.assertIsNone(page.find('.//'+SVG+'image'))

        vectors['parts'][O0+'/body']['ink'] = 'stale'
        with self.db:
            self.db.execute('UPDATE catalog_page_art SET vectors=?',(pack_vectors(vectors),))
        with closing(read(self.database)) as db:
            page = ET.fromstring(page_svg(db,'3',colored=True))
        self.assertEqual(page.get('data-pending'),'3')
        self.assertIsNone(page.find('.//'+SVG+"use[@data-part='body']"))
