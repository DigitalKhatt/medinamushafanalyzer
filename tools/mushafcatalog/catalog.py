#!/usr/bin/env python3
"""Build, verify, search or serve the 1441 Hafs native corpus."""
import argparse
from contextlib import closing
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import sqlite3
from tempfile import NamedTemporaryFile
from urllib.parse import parse_qsl, urlsplit

from search import KINDS, body_source, detail, facets, image, search
from store import DB, HERE, META, REPORT, ROOT, build, export_bodies, import_bodies, meta, read, require, verify
from page_view import page_svg


def serve(database, port, host='127.0.0.1'):
    with closing(read(database)) as db:
        require(meta(db,META), 'Build the native catalog first.')
        require(any(row[1]=='source_sha' for row in db.execute('PRAGMA table_info(catalog_page_art)')),
                'Run build with an AI/PDF report before serving this catalog.')
    assets = {'/':('index.html','text/html'), '/catalog.css':('catalog.css','text/css'), '/theme.css':('theme.css','text/css'),
              '/viewer.html':('viewer.html','text/html'), '/viewer.css':('viewer.css','text/css'),
              **{'/'+name:(name,'text/javascript') for name in ('catalog.js','result-grid.js','inspector.js','ui.js','viewer.js')}}
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            try:
                route = urlsplit(self.path)
                params = dict(parse_qsl(route.query, max_num_fields=35))
                if route.path in assets:
                    name,mime = assets[route.path]
                    content = (HERE/name).read_bytes()
                else:
                    with closing(read(database)) as db:
                        db.execute('BEGIN')
                        if route.path=='/api/search': data = search(db,params)
                        elif route.path=='/api/facets': data = facets(db)
                        elif route.path=='/api/statistics': data = search(db,params,statistics=True)
                        elif route.path=='/api/word': data = detail(db,params.get('id',''))
                        elif route.path=='/api/body-source': data = body_source(db,params.get('id',''))
                        elif route.path=='/api/viewer-pages':
                            data = [int(r[0][4:]) for r in db.execute('SELECT page_id FROM catalog_pages ORDER BY page_id')]
                        elif route.path=='/api/page.svg':
                            data = page_svg(db,params.get('page',''),params.get('color')=='1',
                                            interior=params.get('interior')=='1',
                                            kashida=params.get('kashida','middle'))
                        elif route.path=='/api/image':
                            data = image(db, params.get('kind','subwords'),params.get('id',''),params.get('context','crop'),
                                         pair=params.get('pair')=='1',left=params.get('kleft')=='1',
                                         right=params.get('kright')=='1',dark=params.get('theme')=='dark',
                                         marks=params.get('marks')!='0')
                        else: raise KeyError('Unknown endpoint')
                    if route.path in ('/api/image','/api/page.svg'): content,mime = data.encode(),'image/svg+xml'
                    else: content,mime = json.dumps(data,ensure_ascii=False).encode(),'application/json'
                self.send_response(HTTPStatus.OK)
                self.send_header('Content-Type',mime+'; charset=utf-8')
                self.send_header('Content-Length',str(len(content)))
                self.send_header('X-Content-Type-Options','nosniff')
                self.end_headers()
                self.wfile.write(content)
            except (BrokenPipeError,ConnectionResetError):
                pass
            except KeyError as exc:
                self.send_error(HTTPStatus.NOT_FOUND,str(exc))
            except (ValueError,TypeError) as exc:
                self.send_error(HTTPStatus.BAD_REQUEST,str(exc))
            except sqlite3.Error as exc:
                self.send_error(HTTPStatus.INTERNAL_SERVER_ERROR,str(exc))
    with ThreadingHTTPServer((host,port),Handler) as server:
        print(f'Mushaf Catalog at http://{host}:{server.server_port}/',flush=True)
        server.serve_forever()


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--db',type=Path,default=DB,help=f'Catalog database (default: {DB.relative_to(ROOT)})')
    commands = cli.add_subparsers(dest='command',required=True)
    commands.add_parser('setup',help='Locate or install the pinned Python mark-validation dependencies')
    lock = commands.add_parser('check-inputs',help='Verify source artwork, native templates and validation checkpoints')
    src = commands.add_parser('fresh',help='Regenerate from AI/PDF and QuranTextByWord; compare if report already exists')
    src.add_argument('--report',type=Path,default=REPORT)
    src.add_argument('--pages',default='all')
    src.add_argument('--workers',type=int,choices=range(1,17),default=7)
    src.add_argument('--model-root',type=Path,help='Directory containing models/ for mark validation')
    src.add_argument('--input-lock',type=Path)
    audit = commands.add_parser('audit-marks', help='Export and validate native mark assignments directly from AI/PDF')
    audit.add_argument('--report', type=Path, required=True, help='New mark audit report directory')
    audit.add_argument('--pages', default='3')
    audit.add_argument('--workers', type=int, choices=range(1,17), default=7)
    audit.add_argument('--model-root', type=Path)
    audit.add_argument('--input-lock', type=Path)
    marks = commands.add_parser('replay-marks', help='Run the mark classifier on archived C++ assignments')
    marks.add_argument('--inputs', type=Path, required=True, help='Saved native report inputs directory')
    marks.add_argument('--report', type=Path, required=True, help='New report directory')
    marks.add_argument('--pages', help='Defaults to all pages in the archive')
    marks.add_argument('--workers', type=int, choices=range(1,17), default=4)
    marks.add_argument('--model-root', type=Path, help='Replace the archived classifier checkpoints for this replay')
    for flag in ('--model-root','--input-lock'):
        lock.add_argument(flag,type=Path)
    b = commands.add_parser('build',help='Build from the verified native exports, preserving letter-body imports')
    b.add_argument('--report',type=Path,default=REPORT)
    b.add_argument('--expected-manifest',help=argparse.SUPPRESS)
    b.add_argument('--restore-bodies-from',type=Path,help=argparse.SUPPRESS)
    letters = commands.add_parser('segment-letters',help='Cut native letter bodies and update the catalog')
    letters.add_argument('--pages',default='all',help='All catalog pages, or a selection such as 3,8-12')
    letters.add_argument('--workers',type=int,choices=range(1,17),default=4)
    letters.add_argument('--subwords-file',type=Path,help='Update only these multi-letter subword IDs, one per line')
    letters.add_argument('--log',type=Path,help='Write diagnostics here instead of the default letter-segmentation.log')
    check_cuts = commands.add_parser('check-letter-cuts',help='Compare selected cuts against the catalog without writing to it')
    check_cuts.add_argument('subwords',nargs='+',help='Multi-letter subword IDs such as page069:l15-w6-s1')
    check_cuts.add_argument('--templates',type=Path,default=ROOT/'templates/letter-forms.json',
                            help='Draft template file to check (default: current repository templates)')
    review = commands.add_parser('check-letter-corpus',help='Recompute all letter cuts and make a before/after review without updating the catalog')
    review.add_argument('--pages',default='all')
    review.add_argument('--workers',type=int,choices=range(1,17),default=12)
    review.add_argument('--report',type=Path,required=True,help='Empty output directory for the review and profiles')
    review.add_argument('--templates',type=Path,default=ROOT/'templates/letter-forms.json')
    review.add_argument('--form',help='Check subwords with the selected template’s letters and position')
    page_export = commands.add_parser('export-page',help='Regenerate a standalone SVG page from the catalog')
    page_export.add_argument('page',help='Page number or ID')
    page_export.add_argument('file',type=Path)
    page_export.add_argument('--colored',action='store_true',help='Color each letter together with its marks')
    commands.add_parser('verify')
    commands.add_parser('validate-medial-lam-height',help='Fail if any medial lām body is shorter than 5 page units or cannot be measured')
    commands.add_parser('compact-vectors',help='Remove stored catalog masks after verifying complete native vector coverage')
    replay = commands.add_parser('check-rebuild', help='Rebuild in a temporary empty database and compare all catalog rows and mask bytes')
    replay.add_argument('--bodies', type=Path, help='Replay this saved JSONL instead of exporting current body partitions')
    a = commands.add_parser('attach',help='Import complete body partitions for selected subwords')
    a.add_argument('file',type=Path)
    x = commands.add_parser('export-bodies',help='Export all previously supplied letter partitions')
    x.add_argument('file',type=Path)
    q = commands.add_parser('query',help='Print the same JSON returned by the browser')
    q.add_argument('--kind',choices=KINDS,default='subwords')
    for k in ('q','page','coverage','category','base','class','form','sequence','prev','next','expected','predicted',
              'verdict','role','metric','min','max','sort','limit','offset','match','quality','owner','character',
              'forms','within','minConfidence'):
        q.add_argument('--'+k)
    q.add_argument('--statistics',action='store_true')
    w = commands.add_parser('word',help='Inspect a word, its subwords, character links and geometry')
    w.add_argument('id')
    source = commands.add_parser('body-source',help='Export the native body mask, coordinate system and ordered letter owners')
    source.add_argument('id')
    s = commands.add_parser('serve')
    s.add_argument('--host',default='127.0.0.1',help='IPv4 address to listen on. Use your LAN address for access from another device.')
    s.add_argument('--port',type=int,default=8770)
    args = cli.parse_args()
    if args.command=='setup':
        from environment import validator
        print(validator(install=True))
        return
    if args.command=='fresh':
        from fresh import fresh
        fresh(args.report,args.pages,args.workers,args.model_root,args.input_lock)
        return
    if args.command=='audit-marks':
        from fresh import audit_marks
        audit_marks(args.report,args.pages,args.workers,args.model_root,args.input_lock)
        return
    if args.command=='replay-marks':
        from fresh import replay_marks
        replay_marks(args.inputs, args.report, args.pages, args.workers, args.model_root)
        return
    if args.command=='check-inputs':
        from input_lock import check, validation_models
        pin = check(validation_models(args.model_root),lock=args.input_lock)
        print(f"Verified {pin['pages']} AI/PDF pages, native templates, transcript source and validation models.")
        return
    if args.command=='serve':
        serve(args.db,args.port,args.host)
        return
    if args.command=='check-rebuild':
        from rebuild_check import compare_rebuild
        data = compare_rebuild(args.db,args.bodies)
    elif args.command=='compact-vectors':
        from compact import compact_catalog
        data = compact_catalog(args.db)
    elif args.command=='build':
        data = build(args.db,args.report,args.expected_manifest,args.restore_bodies_from)
    elif args.command=='segment-letters':
        from segment_letters import segment
        selected = args.subwords_file.read_text().split() if args.subwords_file else None
        data = segment(args.db,args.pages,args.workers,selected,args.log)
    elif args.command=='check-letter-cuts':
        from segment_letters import check_cuts
        data = check_cuts(args.db,args.subwords,args.templates)
    elif args.command=='check-letter-corpus':
        from cut_review import review
        data = review(args.db,args.report,args.pages,args.workers,args.templates, form=args.form)
    elif args.command=='export-page':
        target = args.file.resolve()
        require(target!=args.db.resolve() and target.suffix.lower()=='.svg','Choose an SVG output file')
        with closing(read(args.db)) as db:
            db.execute('BEGIN')
            svg = page_svg(db,args.page,args.colored)
        with NamedTemporaryFile(mode='w',encoding='utf-8',dir=target.parent,delete=False) as out:
            try:
                out.write(svg)
                out.close()
                os.replace(out.name,target)
            finally:
                Path(out.name).unlink(missing_ok=True)
        data = {'file':str(target),'page':args.page}
    elif args.command=='attach':
        with closing(sqlite3.connect(args.db.resolve().as_uri()+'?mode=rw',uri=True,timeout=120)) as db, db:
            db.execute('PRAGMA foreign_keys=ON')
            db.execute('BEGIN IMMEDIATE')
            with args.file.open(encoding='utf-8') as stream:
                data = {'importedSubwords':import_bodies(db,stream),'inventory':verify(db)}
    else:
        with closing(read(args.db)) as db:
            db.execute('BEGIN')
            if args.command=='verify': data = verify(db,deep=True)
            elif args.command=='validate-medial-lam-height':
                from letter_validation import validate_medial_lam_height
                data = validate_medial_lam_height(db)
            elif args.command=='word': data = detail(db,args.id)
            elif args.command=='body-source': data = body_source(db,args.id)
            elif args.command=='export-bodies':
                target = args.file.resolve()
                require(target!=args.db.resolve() and target.suffix=='.jsonl','Choose a .jsonl export file')
                with NamedTemporaryFile(mode='w',dir=target.parent,encoding='utf-8',delete=False) as out:
                    try:
                        export_bodies(db,out)
                        out.close()
                        os.replace(out.name,target)
                    finally:
                        Path(out.name).unlink(missing_ok=True)
                data = {'file':str(target),'subwords':db.execute('SELECT count(*) FROM catalog_splits').fetchone()[0]}
            else: data = search(db,{k:v for k,v in vars(args).items() if v},args.statistics)
    print(json.dumps(data,ensure_ascii=False,indent=2))
    if args.command=='validate-medial-lam-height' and not data['passed']:
        raise SystemExit(1)
    if args.command=='check-letter-corpus' and data['issues']:
        raise SystemExit(1)


if __name__=='__main__':
    try:
        main()
    except (ValueError, KeyError, OSError, sqlite3.Error) as error:
        raise SystemExit(str(error))
