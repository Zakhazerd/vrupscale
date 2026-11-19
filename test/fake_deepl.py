#!/usr/bin/env python3
# A stand-in for DeepL's API on this computer, for the end-to-end test of the app's translation
# (tools/ab/translate.sh; the app is pointed at it with VRUPSCALE_DEEPL_URL=http://127.0.0.1:<port>).
#   fake_deepl.py <port> <log file> [status]
# POST /v2/translate answers each text with "<<TEXT IN CAPITALS>>" (or with HTTP <status> if given),
# GET /v2/usage with a count. Every request is appended to the log as a JSON line: the path, the
# Authorization header, and the body as parsed.
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(sys.argv[1])
LOG = sys.argv[2]
STATUS = int(sys.argv[3]) if len(sys.argv) > 3 else 200
sent = 0


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def record(self, body):
        with open(LOG, 'a', encoding='utf-8') as f:
            f.write(json.dumps({'method': self.command, 'path': self.path, 'auth': self.headers.get('Authorization'),
                                'type': self.headers.get('Content-Type'), 'body': body}, ensure_ascii=False) + '\n')

    def answer(self, status, obj):
        data = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.record(None)
        if self.path == '/v2/usage':
            self.answer(200, {'character_count': 1000 + sent, 'character_limit': 500000})
        else:
            self.answer(404, {'message': 'not found'})

    def do_POST(self):
        global sent
        raw = self.rfile.read(int(self.headers.get('Content-Length') or 0))
        try:
            body = json.loads(raw.decode('utf-8'))
        except ValueError:
            body = {'unparsed': raw.decode('utf-8', 'replace')}
        self.record(body)
        if self.path != '/v2/translate' or not isinstance(body.get('text'), list):
            self.answer(400, {'message': 'bad request'})
        elif STATUS != 200:
            self.answer(STATUS, {'message': 'refused by the test'})
        else:
            sent += sum(len(t) for t in body['text'])
            self.answer(200, {'translations': [{'detected_source_language': body.get('source_lang', ''),
                                                'text': '<<' + t.upper() + '>>'} for t in body['text']]})


HTTPServer(('127.0.0.1', PORT), Handler).serve_forever()
