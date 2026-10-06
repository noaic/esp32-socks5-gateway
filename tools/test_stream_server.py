"""Controlled LAN test peer; this is a host tool, never ESP32 firmware.

Serves an 8 MiB deterministic payload, checks uploads and can deliberately reset
one response. No system networking/firewall settings are changed.
"""
import argparse
import hashlib
import json
import socket
import struct
import threading
import math
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BLOCK = bytes(range(256)) * 128
SIZE = 8 * 1024 * 1024
DIGEST = hashlib.sha256(BLOCK * (SIZE // len(BLOCK))).hexdigest()

class PairBarrier:
    """One pair of browsers; align each direction before measuring throughput."""
    def __init__(self, timeout=45, delay=.5):
        self.condition=threading.Condition()
        self.timeout=timeout; self.delay=delay; self.group=None

    def wait(self, client, run_id, stage, pair_id=None):
        if client not in ('A','B') or not isinstance(run_id,str) or not 1<=len(run_id)<=80:
            raise ValueError('choose distinct clients A and B')
        if stage not in ('join','done','1-down','1-up','2-down','2-up','3-down','3-up'):
            raise ValueError('invalid paired stage')
        with self.condition:
            if stage=='join':
                g=self.group
                if g is None or g['closed'] or time.monotonic()-g['touched']>self.timeout:
                    g={'id':uuid.uuid4().hex,'members':{},'stages':{},'closed':False,'failed':False,'touched':time.monotonic()}
                    self.group=g
                if client in g['members'] and g['members'][client]!=run_id:
                    raise ValueError('client label already in use; finish previous pair or wait 45 seconds')
                g['members'][client]=run_id
            else:
                g=self.group
                if not g or g['id']!=pair_id or g['members'].get(client)!=run_id:
                    raise ValueError('paired test expired or mismatched')
            phase=g['stages'].setdefault(stage,{'clients':set(),'start':None})
            phase['clients'].add(client)
            if len(phase['clients'])==2 and phase['start'] is None:
                phase['start']=time.monotonic()+self.delay
                g['touched']=time.monotonic()
                if stage=='done': g['closed']=True
                self.condition.notify_all()
            deadline=time.monotonic()+self.timeout
            while phase['start'] is None and not g['failed']:
                remaining=deadline-time.monotonic()
                if remaining<=0:
                    g['failed']=True; g['closed']=True; self.condition.notify_all(); break
                self.condition.wait(remaining)
            if g['failed']: raise ValueError('other client did not reach this stage within 45 seconds')
            return {'pair_id':g['id'],'wait_ms':max(0,round((phase['start']-time.monotonic())*1000))}

class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def respond(self, body, status=200):
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True
    def do_GET(self):
        self.connection.settimeout(30)
        if self.path == '/':
            self.respond(b'proxy-test-ok\n')
        elif self.path == '/bench':
            body = Path(__file__).with_name('throughput_bench.html').read_bytes()
            label = json.dumps(getattr(self.server, 'label', 'unspecified')).replace('<', '\\u003c')
            body = body.replace(b'"__BENCH_LABEL__"', label.encode())
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path in ('/8m', '/reset'):
            self.send_response(200)
            self.send_header('Content-Length', str(SIZE))
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Connection', 'close')
            self.end_headers()
            count = SIZE // len(BLOCK) if self.path == '/8m' else 2
            try:
                for _ in range(count):
                    self.wfile.write(BLOCK)
                self.wfile.flush()
                if self.path == '/reset':
                    self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii',1,0))
                print(f'GET {self.path}: sent={count * len(BLOCK)} expected_sha256={DIGEST}', flush=True)
            except (OSError, TimeoutError) as exc:
                print(f'GET {self.path}: interrupted={exc}', flush=True)
            self.close_connection = True
        else:
            self.respond(b'not found\n', 404)
    def do_PUT(self):
        self.connection.settimeout(30)
        if self.path != '/upload':
            self.respond(b'not found\n',404); return
        try:
            length = int(self.headers.get('Content-Length','-1'))
        except ValueError:
            length = -1
        if not 0 <= length <= SIZE:
            self.respond(b'Content-Length must be between 0 and 8 MiB\n',400); return
        if self.headers.get('Transfer-Encoding'):
            self.respond(b'chunked upload not supported\n',400); return
        remaining = length; digest = hashlib.sha256()
        while remaining:
            data = self.rfile.read(min(32768, remaining))
            if not data:
                self.close_connection = True; return
            digest.update(data); remaining -= len(data)
        result = {'bytes':length, 'sha256':digest.hexdigest(), 'matches_8m':length == SIZE and digest.hexdigest() == DIGEST}
        self.respond((json.dumps(result)+'\n').encode())
        print(f'PUT /upload: {result}', flush=True)

    def do_POST(self):
        self.connection.settimeout(10)
        if self.path not in ('/results','/pair'):
            self.respond(b'not found\n', 404); return
        try:
            length = int(self.headers.get('Content-Length', '-1'))
            if not 0 < length <= 16384 or self.headers.get('Transfer-Encoding'):
                raise ValueError('invalid report length')
            report = json.loads(self.rfile.read(length))
            if not isinstance(report, dict): raise ValueError('object required')
            if self.path=='/pair':
                result=self.server.pair_barrier.wait(report.get('client_id'),report.get('run_id'),report.get('stage'),report.get('pair_id'))
                self.respond(json.dumps(result).encode()); return
            if report.get('phase') not in ('start', 'round', 'complete', 'error'):
                raise ValueError('invalid phase')
            run_id = report.get('run_id')
            if not isinstance(run_id, str) or not 1 <= len(run_id) <= 80:
                raise ValueError('invalid run_id')
            streams = report.get('streams', 1)
            if type(streams) is not int or streams not in (1, 4):
                raise ValueError('invalid streams')
            rounds = report.get('rounds', [])
            if not isinstance(rounds, list) or len(rounds) > 3: raise ValueError('invalid rounds')
            for item in rounds:
                if not isinstance(item, dict): raise ValueError('invalid round')
                for key in ('down', 'up', 'download_ms', 'upload_ms'):
                    value = item.get(key)
                    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
                        raise ValueError('invalid measurement')
            if report['phase'] == 'complete' and len(rounds) != 3:
                raise ValueError('three rounds required')
            record = {'received_at': datetime.now(timezone.utc).isoformat(),
                      'firmware_label': getattr(self.server, 'label', 'unspecified'),
                      'peer': self.client_address[0], 'user_agent': self.headers.get('User-Agent', '')[:256],
                      'report': report}
            with self.server.results_lock:
                with self.server.results_path.open('a', encoding='utf-8') as file:
                    file.write(json.dumps(record, ensure_ascii=False, allow_nan=False) + '\n')
            self.respond(b'{"saved":true}\n')
            print(f'BENCH_RESULT {json.dumps(record, ensure_ascii=False)}', flush=True)
        except (ValueError, TypeError, UnicodeError):
            self.respond(b'invalid report\n', 400)
        except OSError as exc:
            print(f'BENCH_RESULT save failed: {exc}', flush=True)
            self.respond(b'could not save report\n', 500)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port',type=int,default=18080)
    parser.add_argument('--bind',default='0.0.0.0')
    parser.add_argument('--results', type=Path, default=Path('/tmp/esp32-bench-results.jsonl'))
    parser.add_argument('--label', default='unspecified')
    args = parser.parse_args()
    with ThreadingHTTPServer((args.bind,args.port),Handler) as server:
        server.results_path = args.results
        server.label = args.label
        server.results_lock = threading.Lock()
        server.pair_barrier = PairBarrier()
        print(f'Test peer listening port={args.port}; size={SIZE}; SHA256={DIGEST}',flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
