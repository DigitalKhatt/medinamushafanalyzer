"""Remove raster payloads once the native vectors cover the published catalog."""
from contextlib import closing
from pathlib import Path
import sqlite3

from page_view import decode_vectors, mark_outlines, matching_part
from store import INK_REF, META, VECTOR_STORAGE, ink_reference, meta, put_meta, require, verify


def verify_vectors(db, *, stripped=True):
    pages = db.execute('''SELECT c.page_id,a.vectors FROM catalog_pages c JOIN pages p USING(page_id)
        LEFT JOIN catalog_page_art a ON a.page_id=c.page_id AND a.report_sha=c.report_sha AND a.source_sha=p.source_sha
        ORDER BY c.page_id''')
    for pid,blob in pages:
        require(blob, f'{pid}: publish native vector artwork first')
        vectors = decode_vectors(blob)
        require(vectors.get('pageArt', {}).get('paint'), f'{pid}: missing original vector artwork')
        parts = db.execute('''SELECT p.owner_id,p.role,p.runs FROM catalog_subwords s
            JOIN catalog_parts p ON p.subword_id=s.id WHERE s.page_id=?''',(pid,))
        for owner,role,ink in parts:
            outline = matching_part(vectors,owner,role,ink)
            require(outline and outline.get('d') and (role=='body' or outline.get('caps')),
                    f'{owner}/{role}: no matching native vector')
            if stripped:
                require(ink.startswith(INK_REF) and len(ink)==len(INK_REF)+32,
                        f'{owner}/{role}: unexpected stored raster data')
        raster_marks = set(vectors.get('rasterMarks', []))
        for mark,ids in db.execute('SELECT id,path_ids FROM native_marks WHERE page_id=? AND runs IS NOT NULL',(pid,)):
            require(mark_outlines(vectors,raster_marks,mark,ids), f'{mark}: no matching native mark vector')
    if stripped:
        for table in ('catalog_subwords','native_marks','native_mark_words'):
            require(not db.execute(f'SELECT 1 FROM {table} WHERE length(runs)>0 LIMIT 1').fetchone(),
                    f'{table}: unexpected stored raster data')


def compact_catalog(database):
    path = database.resolve()
    before = path.stat().st_size
    with closing(sqlite3.connect(path.as_uri()+'?mode=rw',uri=True,timeout=120)) as db:
        db.execute('PRAGMA foreign_keys=ON')
        with db:
            db.execute('BEGIN IMMEDIATE')
            counts = verify(db)
            if not meta(db,VECTOR_STORAGE):
                verify_vectors(db,stripped=False)
                report = Path(meta(db,META)['reportDirectory'])
                for (pid,) in db.execute('SELECT page_id FROM catalog_pages'):
                    require((report/f'{pid}.json').is_file(), f'{pid}: retain the source validation report before compacting')
                db.create_function('ink_reference',1,ink_reference,deterministic=True)
                db.execute('UPDATE catalog_parts SET runs=ink_reference(runs)')
                for table in ('catalog_subwords','native_marks','native_mark_words'):
                    db.execute(f"UPDATE {table} SET runs=X'' WHERE runs IS NOT NULL")
                put_meta(db,VECTOR_STORAGE,True)
                require(counts==verify(db), 'Inventory changed during compaction')
            else:
                verify_vectors(db)
        db.execute('VACUUM')
        db.execute('PRAGMA wal_checkpoint(TRUNCATE)').fetchone()
    return dict(beforeBytes=before,afterBytes=path.stat().st_size,pages=counts['pages'])
