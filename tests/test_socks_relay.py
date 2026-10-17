"""Host integration tests execute the production nonblocking C event loop."""
import os, pathlib, socket, subprocess, threading, time, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]

def exact(s, n):
    result = b''
    while len(result) < n:
        part = s.recv(n - len(result))
        if not part: raise EOFError()
        result += part
    return result

class RelayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = cls.build_binary('relay',1500)
    @classmethod
    def build_binary(cls,name,idle_ms):
        binary = ROOT / 'tests/.out' / name
        binary.parent.mkdir(exist_ok=True)
        subprocess.run(['gcc','-std=c11','-D_POSIX_C_SOURCE=200809L','-DSOCKS_HANDSHAKE_MS=500',
                        f'-DSOCKS_IDLE_MS={idle_ms}','-DSOCKS_PRESSURE_IDLE_MS=700','-pthread','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),
                        str(ROOT/'tests/relay_harness.c'),str(ROOT/'tests/shutdown_closed_shim.c'),
                        str(ROOT/'main/socks_relay.c'),str(ROOT/'main/ap_clients.c'),'-Wl,--wrap=shutdown','-Wl,--wrap=send','-o',str(binary)],check=True)
        return binary
    def run_case(self, behavior, payload=b'hello', env=None, halfclose=True):
        upstream = socket.socket(); upstream.bind(('127.0.0.1',0)); upstream.listen()
        errors=[]
        def server():
            try:
                with upstream.accept()[0] as s:
                    s.settimeout(4)
                    behavior(s)
            except Exception as e: errors.append(e)
        proc=subprocess.Popen([str(self.binary),'127.0.0.1',str(upstream.getsockname()[1])],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env)
        worker=threading.Thread(target=server); worker.start()
        try:
            port=int(proc.stdout.readline())
            with socket.create_connection(('127.0.0.1',port)) as c:
                c.settimeout(4); c.sendall(payload)
                if halfclose: c.shutdown(socket.SHUT_WR)
                received=b''; reset=False
                try:
                    while True:
                        data=c.recv(16384)
                        if not data: break
                        received+=data
                except ConnectionResetError: reset=True
            worker.join(5); self.assertFalse(worker.is_alive())
            if errors:
                proc.terminate(); _,logs=proc.communicate(timeout=3)
                self.fail(f"server errors={errors}; client_bytes={len(received)} reset={reset}; loop logs={logs}")
            return received, reset
        finally:
            proc.terminate(); proc.communicate(timeout=3); upstream.close()
    def handshake(self,s,split=False):
        self.assertEqual(exact(s,3),b'\x05\x01\x00')
        for part in ([b'\x05',b'\x00'] if split else [b'\x05\x00']):
            s.sendall(part)
            if split: time.sleep(.01)
        self.assertEqual(exact(s,10),b'\x05\x01\x00\x01\x01\x02\x03\x04\x01\xbb')
    def test_client_disconnect_isolated_and_reconnect_works(self):
        upstream=socket.socket(); upstream.bind(('127.0.0.1',0)); upstream.listen(4)
        workers=[]; errors=[]; sockets=[]
        def echo(s):
            try:
                with s:
                    s.settimeout(4); self.handshake(s)
                    s.sendall(b'\x05\x00\x00\x01'+bytes(6))
                    while True:
                        data=s.recv(1024)
                        if not data: break
                        s.sendall(data)
            except (ConnectionResetError,BrokenPipeError): pass
            except Exception as e: errors.append(e)
        proc=subprocess.Popen([str(self.binary),'127.0.0.1',str(upstream.getsockname()[1])],
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
            env={**os.environ,'RELAY_TEST_CLIENTS':'1'})
        try:
            port=int(proc.stdout.readline())
            def connect(ip,source_port=0):
                s=socket.socket(); s.bind((ip,source_port)); s.connect(('127.0.0.1',port)); s.settimeout(2); sockets.append(s)
                worker=threading.Thread(target=echo,args=(upstream.accept()[0],)); worker.start(); workers.append(worker)
                s.sendall(b'hello'); self.assertEqual(exact(s,5),b'hello'); return s
            a=connect('127.0.0.2'); b=connect('127.0.0.3',a.getsockname()[1])
            proc.stdin.write('a'); proc.stdin.flush(); time.sleep(.15)
            try: self.assertEqual(a.recv(1),b'')
            except ConnectionResetError: pass
            b.sendall(b'keep'); self.assertEqual(exact(b,4),b'keep')
            proc.stdin.write('b'); proc.stdin.flush(); time.sleep(.05)
            fresh=connect('127.0.0.2'); fresh.sendall(b'new'); self.assertEqual(exact(fresh,3),b'new')
            b.sendall(b'still'); self.assertEqual(exact(b,5),b'still')
        finally:
            for s in sockets: s.close()
            proc.terminate(); proc.communicate(timeout=3)
            for w in workers: w.join(4)
            upstream.close()
        self.assertEqual(errors,[])
    def test_fragmented_handshake_large_echo_and_early_halfclose(self):
        payload=bytes(range(256))*32768
        def behavior(s):
            self.handshake(s,True)
            for b in b'\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00':
                s.sendall(bytes([b])); time.sleep(.001)
            # Read the entire request before responding; early client FIN must propagate.
            data=bytearray()
            while True:
                part=s.recv(4096)
                if not part: break
                data.extend(part)
            self.assertEqual(data,payload)
            s.sendall(data); s.shutdown(socket.SHUT_WR)
        data,reset=self.run_case(behavior,payload); self.assertEqual(data,payload); self.assertFalse(reset)
    def test_coalesced_connect_reply_and_payload(self):
        replies = {
            'IPv4': b'\x05\x00\x00\x01'+bytes(6),
            'IPv6': b'\x05\x00\x00\x04'+socket.inet_pton(socket.AF_INET6,'::ffff:192.0.2.1')+b'\x01\xbb',
            'domain': b'\x05\x00\x00\x03\x0dproxy.example\x01\xbb',
            'one-byte domain': b'\x05\x00\x00\x03\x01x\x01\xbb',
            '255-byte domain field': b'\x05\x00\x00\x03\xff'+b'x'*255+b'\x01\xbb',
        }
        banner=b'banner\x00\x05\x00\x00\x01'+bytes(range(256))*4
        for name,reply in replies.items():
            with self.subTest(bound_address=name):
                def behavior(s):
                    self.handshake(s)
                    s.sendall(reply+banner)
                    self.assertEqual(exact(s,5),b'hello'); self.assertEqual(s.recv(1),b'')
                    s.shutdown(socket.SHUT_WR)
                data,reset=self.run_case(behavior)
                self.assertEqual(data,banner); self.assertFalse(reset)
    def test_fragmented_variable_connect_reply_and_payload(self):
        replies = [b'\x05\x00\x00\x04'+socket.inet_pton(socket.AF_INET6,'::1')+b'\x01\xbb',
                   b'\x05\x00\x00\x03\x0dproxy.example\x01\xbb',
                   b'\x05\x00\x00\x03\xff'+b'x'*255+b'\x01\xbb']
        for reply in replies:
            with self.subTest(reply_bytes=len(reply)):
                def behavior(s):
                    self.handshake(s,True)
                    # Split every header byte and the final port byte. Keep
                    # maximum-length names in bounded chunks so this checks
                    # framing rather than the deliberately short test deadline.
                    parts=[reply[i:i+1] for i in range(5)]
                    parts += [reply[i:min(i+31,len(reply)-1)] for i in range(5,len(reply)-1,31)]
                    for part in parts:
                        s.sendall(part); time.sleep(.001)
                    s.sendall(reply[-1:]+b'banner')
                    self.assertEqual(exact(s,5),b'hello'); self.assertEqual(s.recv(1),b'')
                    s.shutdown(socket.SHUT_WR)
                data,reset=self.run_case(behavior)
                self.assertEqual(data,b'banner'); self.assertFalse(reset)
    def test_full_duplex_with_slow_reader_and_backpressure(self):
        request=bytes(range(256))*4096
        response=bytes(reversed(range(256)))*8192
        upstream=socket.socket(); upstream.bind(('127.0.0.1',0)); upstream.listen()
        errors=[]
        def server():
            try:
                with upstream.accept()[0] as s:
                    s.settimeout(8); self.handshake(s)
                    s.sendall(b'\x05\x00\x00\x01'+bytes(6))
                    def write_response():
                        try: s.sendall(response); s.shutdown(socket.SHUT_WR)
                        except Exception as e: errors.append(e)
                    writer=threading.Thread(target=write_response); writer.start()
                    self.assertEqual(exact(s,len(request)),request)
                    self.assertEqual(s.recv(1),b'')
                    writer.join(8); self.assertFalse(writer.is_alive())
            except Exception as e: errors.append(e)
        proc=subprocess.Popen([str(self.binary),'127.0.0.1',str(upstream.getsockname()[1])],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
                              env={**os.environ,"RELAY_TEST_SMALL_SEND_BUFFER":"1"})
        worker=threading.Thread(target=server); worker.start()
        try:
            port=int(proc.stdout.readline())
            with socket.create_connection(('127.0.0.1',port)) as c:
                c.settimeout(8); c.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,32768)
                def write_request():
                    try: c.sendall(request); c.shutdown(socket.SHUT_WR)
                    except Exception as e: errors.append(e)
                writer=threading.Thread(target=write_request); writer.start()
                # Force downstream backpressure while the upstream reader keeps
                # processing client data. Both directions must preserve order.
                time.sleep(.2)
                received=bytearray()
                while True:
                    data=c.recv(1537)
                    if not data: break
                    received.extend(data)
                self.assertEqual(bytes(received),response)
                writer.join(8); self.assertFalse(writer.is_alive())
            worker.join(8); self.assertFalse(worker.is_alive()); self.assertEqual(errors,[])
        finally:
            proc.terminate(); proc.communicate(timeout=3); upstream.close()
    def test_already_closed_write_half_preserves_response(self):
        response=bytes(range(256))*4096
        def behavior(s):
            self.handshake(s); s.sendall(b'\x05\x00\x00\x01'+bytes(6))
            self.assertEqual(exact(s,5),b'hello'); self.assertEqual(s.recv(1),b'')
            s.sendall(response); s.shutdown(socket.SHUT_WR)
        data,reset=self.run_case(behavior,env={**os.environ,"RELAY_TEST_SHUTDOWN_CLOSED":"1"})
        self.assertEqual(data,response); self.assertFalse(reset)
    def test_auth_rejected_resets_client(self):
        def behavior(s):
            exact(s,3); s.sendall(b'\x05\xff')
        data,reset=self.run_case(behavior); self.assertEqual(data,b''); self.assertTrue(reset)
    def test_connect_failure_and_unsupported_reply(self):
        for header in [b'\x05\x05\x00\x01',b'\x04\x00\x00\x01',b'\x05\x00\x01\x01',
                       b'\x05\x00\x00\x00',b'\x05\x00\x00\x02',b'\x05\x00\x00\xff',
                       b'\x05\x00\x00\x03\x00'+b'x'*256]:
            with self.subTest(header=header):
                def behavior(s):
                    self.handshake(s); s.sendall(header)
                data,reset=self.run_case(behavior); self.assertEqual(data,b''); self.assertTrue(reset)
    def test_truncated_connect_reply_never_forwards_bound_address(self):
        replies=[b'', b'\x05', b'\x05\x00\x00\x01'+bytes(5),
                 b'\x05\x00\x00\x04'+bytes(17), b'\x05\x00\x00\x03',
                 b'\x05\x00\x00\x03\x04abc', b'\x05\x00\x00\x03\x04name\x01',
                 b'\x05\x00\x00\x03\xff'+b'x'*254+b'\x01\xbb']
        for reply in replies:
            with self.subTest(reply=reply):
                def behavior(s):
                    self.handshake(s)
                    s.sendall(reply); s.shutdown(socket.SHUT_WR)
                data,reset=self.run_case(behavior)
                self.assertEqual(data,b''); self.assertTrue(reset)
    def test_incomplete_connect_reply_keeps_handshake_deadline(self):
        def behavior(s):
            self.handshake(s)
            s.sendall(b'\x05\x00\x00\x03\xff'+b'x'*100)
            time.sleep(.8)
        data,reset=self.run_case(behavior)
        self.assertEqual(data,b''); self.assertTrue(reset)
    def test_stalled_handshake_timeout(self):
        def behavior(s):
            exact(s,3); time.sleep(.8)
        data,reset=self.run_case(behavior); self.assertEqual(data,b''); self.assertTrue(reset)
    def test_upstream_reset(self):
        def behavior(s):
            import struct
            self.handshake(s); s.sendall(b'\x05\x00\x00\x01'+bytes(6))
            s.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
        data,reset=self.run_case(behavior); self.assertEqual(data,b''); self.assertTrue(reset)
    def test_idle_halfclosed_relay_fin_without_reset(self):
        def behavior(s):
            self.handshake(s); s.sendall(b'\x05\x00\x00\x01'+bytes(6))
            self.assertEqual(exact(s,5),b'hello'); self.assertEqual(s.recv(1),b'')
            time.sleep(1.8)
        data,reset=self.run_case(behavior); self.assertEqual(data,b''); self.assertFalse(reset)

    def test_idle_keepalive_fin_on_both_sides(self):
        def behavior(s):
            self.handshake(s); s.sendall(b'\x05\x00\x00\x01'+bytes(6))
            self.assertEqual(exact(s,5),b'hello')
            # No client FIN was sent. The relay's idle expiry closes the write
            # half towards the upstream, while the client must see clean EOF.
            self.assertEqual(s.recv(1),b'')
        data,reset=self.run_case(behavior,halfclose=False)
        self.assertEqual(data,b''); self.assertFalse(reset)

    def test_idle_with_unsent_payload_still_resets(self):
        def behavior(s):
            self.handshake(s)
            s.sendall(b'\x05\x00\x00\x01'+bytes(6)+b'idle-blocked!')
            self.assertEqual(exact(s,5),b'hello'); self.assertEqual(s.recv(1),b'')
            time.sleep(1.8)
        data,reset=self.run_case(behavior,env={**os.environ,"RELAY_TEST_STALL_PAYLOAD":"1"})
        self.assertEqual(data,b''); self.assertTrue(reset)

    def test_recovery_after_upstream_disconnect_in_same_event_loop(self):
        # Keep the production loop alive across an interrupted connection, a
        # stalled greeting, and eight succeeding requests. This catches stale
        # descriptors/state that isolated one-request tests cannot detect.
        upstream=socket.socket(); upstream.bind(('127.0.0.1',0)); upstream.listen()
        proc=subprocess.Popen([str(self.binary),'127.0.0.1',str(upstream.getsockname()[1])],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        errors=[]
        def server():
            try:
                for attempt in range(10):
                    with upstream.accept()[0] as s:
                        s.settimeout(4)
                        if attempt == 0:
                            self.handshake(s); s.sendall(b'\x05\x00\x00\x01'+bytes(6))
                            self.assertEqual(exact(s,5),b'hello')
                            import struct
                            s.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
                        elif attempt == 1:
                            self.assertEqual(exact(s,3),b'\x05\x01\x00'); time.sleep(.7)
                        else:
                            self.handshake(s); s.sendall(b'\x05\x00\x00\x01'+bytes(6))
                            self.assertEqual(exact(s,5),b'hello'); self.assertEqual(s.recv(1),b'')
                            s.sendall(b'world'); s.shutdown(socket.SHUT_WR)
            except Exception as e: errors.append(e)
        worker=threading.Thread(target=server); worker.start()
        try:
            port=int(proc.stdout.readline())
            for attempt in range(10):
                with socket.create_connection(('127.0.0.1',port)) as c:
                    c.settimeout(4); c.sendall(b'hello'); c.shutdown(socket.SHUT_WR)
                    received=b''; reset=False
                    try:
                        while True:
                            data=c.recv(4096)
                            if not data: break
                            received+=data
                    except ConnectionResetError: reset=True
                    if attempt < 2: self.assertTrue(reset); self.assertEqual(received,b'')
                    else: self.assertFalse(reset); self.assertEqual(received,b'world')
                # Close and retire happen immediately after FIN propagation.
                # Let FIN drain before the next request.
                time.sleep(.05)
            worker.join(5); self.assertFalse(worker.is_alive()); self.assertEqual(errors,[])
            self.assertIsNone(proc.poll())
        finally:
            proc.terminate(); proc.communicate(timeout=3); upstream.close()

    def test_parallel_sessions_and_network_client_reset_recovery(self):
        upstream=socket.socket(); upstream.bind(('127.0.0.1',0)); upstream.listen(16); upstream.settimeout(.2)
        stop=threading.Event(); errors=[]; workers=[]
        def echo(s):
            try:
                with s:
                    s.settimeout(4); self.handshake(s)
                    s.sendall(b'\x05\x00\x00\x01'+bytes(6))
                    while True:
                        data=s.recv(4096)
                        if not data: break
                        s.sendall(data)
            except (ConnectionResetError, BrokenPipeError): pass
            except Exception as e: errors.append(e)
        def acceptor():
            while not stop.is_set():
                try: s,_=upstream.accept()
                except socket.timeout: continue
                worker=threading.Thread(target=echo,args=(s,)); workers.append(worker); worker.start()
        proc=subprocess.Popen([str(self.binary),'127.0.0.1',str(upstream.getsockname()[1])],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        server=threading.Thread(target=acceptor); server.start(); clients=[]
        def opened(port,n):
            c=socket.create_connection(('127.0.0.1',port)); c.settimeout(3); clients.append(c)
            payload=('session-%d'%n).encode(); c.sendall(payload)
            self.assertEqual(exact(c,len(payload)),payload)
            return c
        def command(ch):
            proc.stdin.write(ch+'\n'); proc.stdin.flush(); time.sleep(.15)
        def closed(c):
            try: self.assertEqual(c.recv(1),b'')
            except ConnectionResetError: pass
        try:
            port=int(proc.stdout.readline())
            # All 64 remain open: catches single-session and undersized loops.
            batch=[opened(port,n) for n in range(64)]
            for n,c in enumerate(batch):
                payload=bytes([n])*4096; c.sendall(payload); self.assertEqual(exact(c,len(payload)),payload)
            pending=socket.create_connection(('127.0.0.1',port)); clients.append(pending)
            pending.settimeout(.15); pending.sendall(b'overflow')
            # At capacity the new connection waits; it must not receive a reset.
            with self.assertRaises(socket.timeout): pending.recv(1)
            batch[0].shutdown(socket.SHUT_WR); self.assertEqual(batch[0].recv(1),b'')
            pending.settimeout(3); self.assertEqual(exact(pending,8),b'overflow')
            batch.append(pending)
            command('r')
            for c in batch: closed(c)
            c=opened(port,9)
            command('d'); closed(c)
            blocked=socket.create_connection(('127.0.0.1',port)); blocked.settimeout(.15); clients.append(blocked)
            blocked.sendall(b'waiting-for-network')
            with self.assertRaises(socket.timeout): blocked.recv(1)
            command('u')
            blocked.settimeout(3); self.assertEqual(exact(blocked,19),b'waiting-for-network')
            blocked.shutdown(socket.SHUT_WR); self.assertEqual(blocked.recv(1),b'')
            c=opened(port,10); c.shutdown(socket.SHUT_WR); self.assertEqual(c.recv(1),b'')
            self.assertIsNone(proc.poll())
        finally:
            for c in clients: c.close()
            proc.terminate(); _,logs=proc.communicate(timeout=3)
            stop.set(); server.join(2)
            for worker in workers: worker.join(4)
            upstream.close()
        self.assertEqual(errors,[],logs)

    def test_capacity_reclaims_old_idle_but_preserves_active_flow(self):
        upstream=socket.socket(); upstream.bind(('127.0.0.1',0)); upstream.listen(64); upstream.settimeout(.1)
        stop=threading.Event(); errors=[]; workers=[]
        def echo(s):
            try:
                with s:
                    s.settimeout(4); self.handshake(s)
                    s.sendall(b'\x05\x00\x00\x01'+bytes(6))
                    while not stop.is_set():
                        data=s.recv(4096)
                        if not data: return
                        s.sendall(data)
            except (ConnectionResetError, BrokenPipeError): pass
            except Exception as e: errors.append(e)
        def acceptor():
            while not stop.is_set():
                try: s,_=upstream.accept()
                except socket.timeout: continue
                worker=threading.Thread(target=echo,args=(s,)); workers.append(worker); worker.start()
        proc=subprocess.Popen([str(self.binary),'127.0.0.1',str(upstream.getsockname()[1])],
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        server=threading.Thread(target=acceptor); server.start(); clients=[]
        try:
            port=int(proc.stdout.readline())
            for _ in range(64):
                c=socket.create_connection(('127.0.0.1',port)); c.settimeout(3); clients.append(c)
                c.sendall(b'first'); self.assertEqual(exact(c,5),b'first')
            # Keep one session busy while the other 63 cross the scaled 700ms
            # pressure TTL. Normal idle expiry is later, at 1500ms.
            for _ in range(8):
                time.sleep(.1); clients[-1].sendall(b'active')
                self.assertEqual(exact(clients[-1],6),b'active')
            newcomer=socket.create_connection(('127.0.0.1',port)); newcomer.settimeout(.5); clients.append(newcomer)
            newcomer.sendall(b'new-page'); self.assertEqual(exact(newcomer,8),b'new-page')
            self.assertEqual(clients[0].recv(1),b'')  # Oldest closes cleanly, without RST.
            clients[-2].sendall(b'still-active'); self.assertEqual(exact(clients[-2],12),b'still-active')
        finally:
            for c in clients: c.close()
            proc.terminate(); _,logs=proc.communicate(timeout=3)
            stop.set(); server.join(2)
            for worker in workers: worker.join(4)
            upstream.close()
        self.assertEqual(errors,[],logs)
        self.assertIn('capacity idle close',logs)

    def test_capacity_does_not_evict_sessions_awaiting_first_response(self):
        # Test pressure expiry, not normal idle expiry. Keep a wide normal-idle
        # margin; the pressure threshold remains 700ms in this core build.
        binary=self.build_binary('relay_pressure_slow',30000)
        upstream=socket.socket(); upstream.bind(('127.0.0.1',0)); upstream.listen(64); upstream.settimeout(.1)
        stop=threading.Event(); errors=[]; workers=[]
        received=[0]; progress=threading.Condition()
        def silent(s):
            try:
                with s:
                    s.settimeout(4); self.handshake(s)
                    s.sendall(b'\x05\x00\x00\x01'+bytes(6))
                    self.assertEqual(exact(s,5),b'hello')
                    with progress:
                        received[0]+=1
                        progress.notify_all()
                    # No application response yet: a slow server must be
                    # protected from pressure expiry, even after its TTL.
                    stop.wait()  # Keep the connection open until test cleanup.
            except (ConnectionResetError, BrokenPipeError, EOFError): pass
            except Exception as e: errors.append(e)
        def acceptor():
            while not stop.is_set():
                try: s,_=upstream.accept()
                except socket.timeout: continue
                worker=threading.Thread(target=silent,args=(s,)); workers.append(worker); worker.start()
        proc=subprocess.Popen([str(binary),'127.0.0.1',str(upstream.getsockname()[1])],
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        server=threading.Thread(target=acceptor); server.start(); clients=[]
        try:
            port=int(proc.stdout.readline())
            for expected in range(1,65):
                c=socket.create_connection(('127.0.0.1',port)); clients.append(c); c.sendall(b'hello')
                # The harness listener has backlog=1. Wait for each request
                # rather than introducing unrelated TCP SYN-retry delays.
                with progress:
                    self.assertTrue(progress.wait_for(lambda: received[0]>=expected,timeout=2),
                                    'Initial request did not reach the SOCKS5 server')
            self.assertEqual(received[0],64)
            time.sleep(.8)
            newcomer=socket.create_connection(('127.0.0.1',port)); newcomer.settimeout(.15); clients.append(newcomer)
            newcomer.sendall(b'hello')
            with self.assertRaises(socket.timeout): newcomer.recv(1)
            self.assertEqual(received[0],64)
        finally:
            proc.terminate(); _,logs=proc.communicate(timeout=3)
            stop.set()
            for c in clients: c.close()
            server.join(2)
            for worker in workers: worker.join(4)
            upstream.close()
        self.assertEqual(errors,[],logs)
        self.assertNotIn('capacity idle close',logs)
        self.assertNotIn('idle close',logs)

if __name__=='__main__': unittest.main()
