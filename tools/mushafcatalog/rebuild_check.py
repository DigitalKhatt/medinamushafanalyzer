"""Rebuild the consultation in an empty SQLite file and compare the published data, including mask bytes."""
from contextlib import closing
from itertools import zip_longest
from pathlib import Path
import sqlite3
from tempfile import TemporaryDirectory

from store import META, VECTOR_STORAGE, build, export_bodies, import_bodies, meta, read, require, tables, verify


def compare_rebuild(database, body_bundle=None):
    # Use a consistent published snapshot even if a different process imports a new set of cuts.
    with closing(read(database)) as live, TemporaryDirectory(prefix='mushafcatalog-rebuild-') as tmp:
        live.execute('BEGIN')
        info = meta(live,META)
        require(info, 'Build the catalog before comparing a clean rebuild')
        target = Path(tmp)/'clean.sqlite'
        if meta(live,VECTOR_STORAGE):
            require(body_bundle is None,'Mask bundles require a catalog that stores masks')
            build(target,Path(info['reportDirectory']),expected_manifest=info['manifestSha256'],overlay_db=database)
        else:
            bundle = Path(body_bundle) if body_bundle else Path(tmp)/'partitions.jsonl'
            if body_bundle is None:
                with bundle.open('w',encoding='utf-8') as stream:
                    export_bodies(live,stream)
            build(target,Path(info['reportDirectory']),expected_manifest=info['manifestSha256'])
            with closing(sqlite3.connect(target)) as db, db, bundle.open(encoding='utf-8') as stream:
                db.execute('PRAGMA foreign_keys=ON')
                db.execute('BEGIN IMMEDIATE')
                import_bodies(db,stream)
                if 'catalog_page_art' in tables(live):
                    db.executemany('INSERT INTO catalog_page_art VALUES (?,?,?,?)', live.execute('SELECT * FROM catalog_page_art'))
        with closing(read(target)) as clean:
            require(verify(clean,deep=True)==verify(live,deep=True),'Clean and published inventories differ')
            columns = {'pages':'page_id,source_sha,pdf_sha,width,height','words':'id,page_id,line,ordinal,text,bases'}
            keys = {'pages':'page_id','words':'id','native_mark_pages':'page_id','native_mark_words':'word_id',
                    'native_marks':'seq','catalog_pages':'page_id','catalog_coordinates':'page_id','catalog_word_bounds':'word_id',
                    'catalog_subwords':'seq','catalog_owners':'seq','catalog_splits':'subword_id','catalog_parts':'owner_id,role',
                    'catalog_measurements':'owner_id'}
            result = {}
            if 'catalog_page_art' in tables(live):
                keys['catalog_page_art'] = 'page_id'
            for table,order in keys.items():
                sql = f"SELECT {columns.get(table,'*')} FROM {table} ORDER BY {order}"
                count = 0
                for count,(reference,rebuilt) in enumerate(zip_longest(live.execute(sql),clean.execute(sql)),1):
                    require(reference==rebuilt,f'{table}: clean rebuild differs at ordered row {count}')
                result[table] = count
            return dict(passed=True,reportManifestSha256=info['manifestSha256'],identicalRows=result)
