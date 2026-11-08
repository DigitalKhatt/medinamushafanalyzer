"""Local-only letter corpus browser; inference dependencies are not required."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import mimetypes
from pathlib import Path
import secrets
import sqlite3
from urllib.parse import parse_qs, urlparse

from .query import Conflict, Corpus

APP = Path(__file__).resolve().parents[1]/'app'


def handler_for(corpus):
    token = secrets.token_urlsafe(32)

    class Handler(BaseHTTPRequestHandler):
        def send(self, status, body, mime='application/json'):
            if mime=='application/json': body=json.dumps(body,ensure_ascii=False,allow_nan=False).encode()
            elif isinstance(body,str): body=body.encode()
            self.send_response(status)
            self.send_header('Content-Type',mime+'; charset=utf-8' if mime.startswith(('text/','application/json','image/svg')) else mime)
            self.send_header('Content-Length',str(len(body)))
            self.send_header('Cache-Control','no-store')
            self.send_header('X-Content-Type-Options','nosniff')
            self.end_headers(); self.wfile.write(body)

        def do_GET(self):
            try:
                parsed=urlparse(self.path); params=parse_qs(parsed.query)
                first=lambda k,d='': params.get(k,[d])[0]
                if parsed.path=='/api/metadata': self.send(200,dict(corpus.metadata(),token=token))
                elif parsed.path=='/api/search': self.send(200,corpus.search(params))
                elif parsed.path=='/api/detail': self.send(200,corpus.detail(first('kind'),first('id')))
                elif parsed.path=='/api/image.svg': self.send(200,corpus.image(first('kind'),first('id'),first('context')=='1',first('raw')=='1'),'image/svg+xml')
                else:
                    name='letter-corpus.html' if parsed.path=='/' else parsed.path.lstrip('/')
                    if name not in ('letter-corpus.html','letter-corpus.css','letter-corpus.mjs'):
                        self.send(404,{'error':'Not found'}); return
                    self.send(200,(APP/name).read_bytes(),'text/javascript' if name.endswith('.mjs') else mimetypes.guess_type(name)[0])
            except KeyError as error: self.send(404,{'error':str(error)})
            except (ValueError,OSError,sqlite3.Error) as error: self.send(400,{'error':str(error)})

        def do_PUT(self):
            try:
                if self.headers.get('X-Letter-Review-Token') != token:
                    self.send(403,{'error':'Invalid local review token'}); return
                if self.path!='/api/review': self.send(404,{'error':'Not found'}); return
                size=int(self.headers.get('Content-Length','0'))
                if not 0<size<=16384: raise ValueError('Invalid review size')
                if self.headers.get('Content-Type','').split(';')[0]!='application/json': raise ValueError('Expected JSON')
                payload=json.loads(self.rfile.read(size))
                if not isinstance(payload,dict): raise ValueError('Expected review object')
                self.send(200,corpus.review(payload))
            except Conflict as error: self.send(409,{'error':str(error)})
            except (ValueError,TypeError,OSError,sqlite3.Error) as error: self.send(400,{'error':str(error)})

    return Handler


def serve(root, port=8768, reviews=None):
    corpus=Corpus(root,reviews)
    server=ThreadingHTTPServer(('127.0.0.1',port),handler_for(corpus))
    print(f'Mask2Former letter corpus: http://127.0.0.1:{port}',flush=True)
    print(f'Database: {corpus.path}',flush=True)
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally: server.server_close()
