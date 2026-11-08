import copy
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import segment


def decode_png(path):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    offset, compressed = 8, b''
    while offset < len(data):
        length = struct.unpack('>I', data[offset:offset+4])[0]
        kind, payload = data[offset+4:offset+8], data[offset+8:offset+8+length]
        if kind == b'IHDR':
            width,height,depth,_,_,_,_ = struct.unpack('>IIBBBBB',payload)
        if kind == b'IDAT':
            compressed += payload
        offset += length+12
    raw = zlib.decompress(compressed)
    stride = width*(depth//8)
    assert all(raw[y*(stride+1)] == 0 for y in range(height))
    return b''.join(raw[y*(stride+1)+1:(y+1)*(stride+1)] for y in range(height))


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.source, self.data = root/'source', root/'data'
        self.source.mkdir()
        self.svg = self.source/'page003.svg'
        self.svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 5 4"><path d="M0 0h2v2z"/></svg>')
        self.config = {'name':'Test','source_dir':str(self.source)}
        segment.prepare(self.config,self.data)
        self.store = segment.Store(self.config,self.data)
        self.page = self.store.pages['page003']
        self.doc = {'schemaVersion':1,'characterMetadataVersion':1,'pageId':'page003','sourceSha256':self.page['sha256'],
            'revision':0,'raster':{'width':5,'height':4,'viewBox':[0.,0.,5.,4.],
                'scale':1,'threshold':170,'chroma':45},'inkPixels':4,'groups':{
                    'char-0001':{'label':'ن','position':'final','form':'unknown','reviewed':True}},'nextGroup':2,
            'lines':[], 'parts':[
                {'id':'one','runs':[0,2],'area':2,'bbox':[0,0,2,1],'role':'body','owner':'char-0001','line':1,'reviewed':True},
                {'id':'two','runs':[7,1],'area':1,'bbox':[2,1,1,1],'role':'kashida','owner':None,'line':1,'reviewed':False},
                {'id':'three','runs':[19,1],'area':1,'bbox':[4,3,1,1],'role':'unknown','owner':None,'line':None,'reviewed':False},
            ]}

    def tearDown(self):
        self.temp.cleanup()

    def test_save_roundtrip_and_revision_conflict(self):
        saved = self.store.save('page003',self.doc)
        self.assertEqual(saved['revision'],1)
        self.assertEqual(self.store.annotation('page003'),saved)
        self.assertTrue((self.data/'history/page003/revision-000001.json').exists())
        with self.assertRaises(segment.Conflict):
            self.store.save('page003',self.doc)

    def test_landmarks_save_without_changing_ink_or_approval(self):
        saved = self.store.save('page003', self.doc)
        point = {'kind':'contour-turn','x':1.5,'y':0.5,'source':'manual'}
        part = saved['parts'][0]
        part['landmarks'] = [point]
        self.store.save('page003', saved)
        restored = self.store.annotation('page003')
        self.assertEqual(restored['parts'][0], {**self.doc['parts'][0], 'landmarks':[point]})
        restored['parts'][0]['landmarks'][0]['x'] = 3
        with self.assertRaisesRegex(ValueError, 'outside its ink region'):
            self.store.save('page003', restored)

    def test_overlap_rejected(self):
        doc=copy.deepcopy(self.doc)
        doc['parts'][1].update(runs=[1,1],bbox=[1,0,1,1])
        with self.assertRaisesRegex(ValueError,'overlap'):
            segment.validate_annotation(doc,self.page)

    def test_loss_of_ink_rejected(self):
        doc=copy.deepcopy(self.doc)
        doc['parts'].pop()
        with self.assertRaisesRegex(ValueError,'coverage'):
            segment.validate_annotation(doc,self.page)

    def test_moving_original_ink_rejected_even_when_area_is_equal(self):
        saved=self.store.save('page003',self.doc)
        saved['parts'][1].update(runs=[8,1],bbox=[3,1,1,1])
        with self.assertRaisesRegex(ValueError,'moved or deleted'):
            self.store.save('page003',saved)

    def test_changed_source_rejected(self):
        self.svg.write_text('changed')
        with self.assertRaisesRegex(ValueError,'changed'):
            self.store.save('page003',self.doc)

    def test_unknown_ink_cannot_be_approved(self):
        doc=copy.deepcopy(self.doc)
        doc['parts'][2]['reviewed']=True
        with self.assertRaisesRegex(ValueError,'unlabelled'):
            segment.validate_annotation(doc,self.page)

    def test_export_ignores_unreviewed_ink_and_preserves_instance_ownership(self):
        dest=self.data/'export'
        segment.export_page(self.doc,dest,self.page)
        semantic=decode_png(dest/'semantic.png')
        self.assertEqual(list(semantic[:3]),[1,1,0])
        self.assertEqual(semantic[7],255)  # Draft kashida is ignored.
        self.assertEqual(semantic[19],255) # Unknown ink is ignored.
        image=decode_png(dest/'image.png')
        self.assertEqual([image[p] for p in [0,1,7,19]],[0,0,0,0])
        self.assertEqual(image[2],255)
        instances=decode_png(dest/'instances.png')
        self.assertEqual(struct.unpack('>20H',instances)[:3],(1,1,0))
        self.assertEqual(instances[14:16],b'\0\0')

    def test_split_is_page_stable(self):
        before=self.page['split']
        self.assertEqual(before,segment.split_for('page003'))
        self.assertIn(before,{'train','validation','test'})
        self.assertIsNone(self.store.corpus['transcript'])

    def character_doc(self, label='ن', position='final', form='expanded'):
        doc=copy.deepcopy(self.doc)
        doc['characterMetadataVersion']=1
        doc['groups']['char-0001']={'label':label,'position':position,'form':form,'reviewed':True}
        return doc

    def test_all_joining_positions_roundtrip_with_custom_form(self):
        for position in ['initial','medial','final','isolated']:
            doc=self.character_doc(label='ك',position=position,form='zigzag')
            segment.validate_annotation(doc,self.page)
            records=segment.character_records(doc,{'char-0001':1})
            self.assertEqual(records['regions'][0]['character'],'ك')
            self.assertEqual(records['regions'][0]['position'],position)
            self.assertEqual(records['regions'][0]['form'],'zigzag')

    def test_strict_approval_rejects_unassigned_or_incomplete_character(self):
        for change in ['owner','label','position','reviewed']:
            doc=self.character_doc()
            if change=='owner': doc['parts'][0]['owner']=None
            elif change=='label': doc['groups']['char-0001']['label']='لا'
            elif change=='position': doc['groups']['char-0001']['position']='unknown'
            else: doc['groups']['char-0001']['reviewed']=False
            with self.assertRaisesRegex(ValueError,'requires a reviewed character'):
                segment.validate_annotation(doc,self.page)

    def test_unassigned_letter_can_remain_a_draft(self):
        doc=self.character_doc()
        doc['parts'][0].update(owner=None,reviewed=False)
        segment.validate_annotation(doc,self.page)

    def test_invalid_position_and_form_rejected(self):
        for key,value in [('position','beginning'),('form','Zigzag Kaf')]:
            doc=self.character_doc()
            doc['groups']['char-0001'][key]=value
            with self.assertRaises(ValueError): segment.validate_annotation(doc,self.page)

    def test_export_records_character_position_and_form_per_cut(self):
        doc=self.character_doc()
        doc['parts'][1].update(owner='char-0001',reviewed=True)
        dest=self.data/'character-export'
        segment.export_page(doc,dest,self.page)
        records=json.loads((dest/'characters.json').read_text())
        self.assertEqual(records['instances'][0]['targets'],{'character':'ن','position':'final','form':'expanded'})
        self.assertEqual({p['role'] for p in records['regions']},{'body','kashida'})
        self.assertTrue(all(p['character']=='ن' and p['position']=='final' and p['form']=='expanded' for p in records['regions']))
        self.assertTrue(records['instances'][0]['hasReviewedBody'])

    def test_unknown_form_is_not_a_false_standard_form_target(self):
        doc=self.character_doc(form='unknown')
        records=segment.character_records(doc,{'char-0001':1})
        self.assertIsNone(records['instances'][0]['targets']['form'])

    def test_unreviewed_attributes_never_become_character_targets(self):
        doc=self.character_doc()
        doc['groups']['char-0001']['reviewed']=False
        records=segment.character_records(doc,{'char-0001':1})
        self.assertEqual(records['instances'],[])
        self.assertEqual(records['regions'],[])

    def test_partial_instance_is_not_eligible_for_whole_character_classification(self):
        doc=self.character_doc()
        doc['parts'][1]['owner']='char-0001'
        records=segment.character_records(doc,{'char-0001':1})
        self.assertFalse(records['instances'][0]['classificationEligible'])
        doc['parts'][1]['reviewed']=True
        records=segment.character_records(doc,{'char-0001':1})
        self.assertTrue(records['instances'][0]['classificationEligible'])

    def test_legacy_annotations_remain_readable_but_old_clients_cannot_save(self):
        doc=copy.deepcopy(self.doc)
        del doc['characterMetadataVersion']
        doc['groups']['char-0001']={'label':''}
        segment.validate_annotation(doc,self.page)
        with self.assertRaisesRegex(ValueError,'Reload the updated workbench'):
            self.store.save('page003',doc)

    def marks_doc(self):
        doc=copy.deepcopy(self.doc)
        doc['groups']={}
        doc['parts'][0].update(role='mark',owner=None,markType='fatha',reviewed=True)
        doc['parts'][1].update(role='dot',owner=None,markType='single-dot',reviewed=True)
        return doc

    def test_marks_and_dots_save_without_any_character_annotations(self):
        doc=self.marks_doc()
        saved=self.store.save('page003',doc)
        self.assertEqual(saved['groups'],{})
        self.assertTrue(all(p['owner'] is None for p in saved['parts']))
        self.assertEqual(self.store.annotation('page003')['parts'][0]['markType'],'fatha')

    def test_mark_pilot_export_keeps_unlabelled_letters_unknown(self):
        doc=self.marks_doc()
        dest=self.data/'marks-export'
        segment.export_page(doc,dest,self.page)
        pilot=json.loads((dest/'marks.json').read_text())
        self.assertEqual(pilot['classes'],{'other':0,'dot':1,'mark':2})
        self.assertEqual([c['target'] for c in pilot['candidates']],[2,1,None])
        self.assertEqual([c['markType'] for c in pilot['candidates']],['fatha','single-dot',None])
        mask=decode_png(dest/'semantic.png')
        self.assertEqual([mask[p] for p in [0,7,19]],[4,3,255])
        self.assertEqual(decode_png(dest/'instances.png'),b'\0'*40)
        self.assertEqual(json.loads((dest/'characters.json').read_text())['instances'],[])

    def test_other_ink_negative_is_foreground_not_background(self):
        doc=self.marks_doc()
        doc['parts'][1].update(role='other',markType='unknown')
        dest=self.data/'negative-export'
        segment.export_page(doc,dest,self.page)
        pilot=json.loads((dest/'marks.json').read_text())
        self.assertEqual(pilot['candidates'][1]['target'],0)
        self.assertEqual(decode_png(dest/'semantic.png')[7],7)
        self.assertEqual(decode_png(dest/'image.png')[7],0)

    def test_multipart_mark_exports_as_one_region_with_exact_mask(self):
        doc=self.marks_doc()
        doc['parts'][0].update(runs=[0,2,7,1],area=3,bbox=[0,0,3,2],markType='tanwin-fath')
        doc['parts'].pop(1)
        dest=self.data/'multipart-export'
        segment.export_page(doc,dest,self.page)
        pilot=json.loads((dest/'marks.json').read_text())
        self.assertEqual(pilot['candidates'][0]['runs'],[0,2,7,1])
        self.assertEqual(pilot['candidates'][0]['markType'],'tanwin-fath')
        self.assertEqual(len([x for x in pilot['candidates'] if x['target'] is not None]),1)

    def test_invalid_mark_type_rejected(self):
        doc=self.marks_doc()
        doc['parts'][0]['markType']='Some Mark!'
        with self.assertRaisesRegex(ValueError,'Mark type'):
            segment.validate_annotation(doc,self.page)


if __name__=='__main__':
    unittest.main()
