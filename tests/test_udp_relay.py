"""Integration tests run the production per-flow UDP association event loop.
The peer models both a shared old relay and strict dynamic-port associations.
No ESP32 or external network is used.
"""
import contextlib
import os
import pathlib
import select
import socket
import struct
import subprocess
import threading
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def exact(s, size):
    data = b''
    while len(data) < size:
        part = s.recv(size - len(data))
        if not part:
            raise EOFError()
        data += part
    return data


class Peer:
    def __init__(self, shared=False, split=False, zero_ip=False, reject=0,
                 greeting_delay=0, stall_first=0, response_delay=0,
                 dns_response_delay=None):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(64)
        self.listener.settimeout(.1)
        self.port = self.listener.getsockname()[1]
        self.shared = shared
        self.split = split
        self.zero_ip = zero_ip
        self.reject = reject
        self.greeting_delay = greeting_delay
        self.stall_first = stall_first
        self.response_delay = response_delay
        self.dns_response_delay = dns_response_delay
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.associations = []
        self.workers = []
        self.errors = []
        self.received = []
        self.accepted = 0
        self.common = None
        if shared:
            self.common = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.common.bind(('127.0.0.1', self.port))
        self.accept_thread = threading.Thread(target=self.accept_loop)
        self.udp_thread = threading.Thread(target=self.udp_loop)
        self.accept_thread.start()
        self.udp_thread.start()

    def accept_loop(self):
        while not self.stop.is_set():
            try:
                c, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with self.lock:
                self.accepted += 1
                number = self.accepted
            worker = threading.Thread(target=self.control, args=(c, number))
            self.workers.append(worker)
            worker.start()

    def control(self, c, number):
        relay = None
        entry = None
        try:
            c.settimeout(2)
            assert exact(c, 3) == b'\x05\x01\x00'
            delay = 1.1 if number <= self.stall_first else self.greeting_delay
            if self.stop.wait(delay):
                return
            c.sendall(b'\x05\x00')
            request = exact(c, 10)
            assert request[:4] == b'\x05\x03\x00\x01', request
            expected = socket.inet_ntoa(request[4:8]), struct.unpack('!H', request[8:])[0]
            assert expected[0] == '127.0.0.1' and expected[1], expected
            if number <= self.reject:
                c.sendall(b'\x05\x07\x00\x01' + bytes(6))
                return
            relay = self.common or socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            if not self.shared:
                relay.bind(('127.0.0.1', 0))
            entry = {'udp': relay, 'tcp': c, 'expected': expected, 'number': number}
            with self.lock:
                self.associations.append(entry)
            answer = b'\x05\x00\x00\x01' + socket.inet_aton('0.0.0.0' if self.zero_ip else '127.0.0.1')
            answer += struct.pack('!H', relay.getsockname()[1])
            if self.split:
                for b in answer:
                    c.sendall(bytes([b]))
                    time.sleep(.001)
            else:
                c.sendall(answer)
            while not self.stop.is_set():
                rd, _, _ = select.select([c], [], [], .1)
                if rd and not c.recv(1):
                    return
        except (EOFError, ConnectionError, OSError):
            pass  # Closing a generation or idle flow is intentional.
        except Exception as e:
            self.errors.append(e)
        finally:
            if entry:
                with self.lock:
                    if entry in self.associations:
                        self.associations.remove(entry)
            if relay and not self.shared:
                relay.close()
            c.close()

    def udp_loop(self):
        delayed = []
        while not self.stop.is_set():
            with self.lock:
                entries = list(self.associations)
            sockets = list({e['udp'] for e in entries})
            if not sockets:
                self.stop.wait(.01)
                continue
            try:
                rd, _, _ = select.select(sockets, [], [], .01)
            except (OSError, ValueError):
                continue
            for relay in rd:
                try:
                    wire, source = relay.recvfrom(65535)
                except OSError:
                    continue
                # A shared relay can become readable after a new control
                # registered while select was blocked; use the live registry.
                with self.lock:
                    entry = next((e for e in self.associations if e['udp'] is relay and e['expected'] == source), None)
                if entry is None:
                    self.errors.append(AssertionError(f'unassociated source endpoint {source}'))
                    continue
                if len(wire) < 10 or wire[:4] != b'\0\0\0\1':
                    self.errors.append(AssertionError(f'bad UDP header {wire[:10]!r}'))
                    continue
                with self.lock:
                    self.received.append((entry['number'], source, wire))
                payload = wire[10:]
                if wire[8:10] == b'\0\x35' and len(payload) >= 12:
                    wire = wire[:12] + bytes([wire[12] | 0x80]) + wire[13:]
                if payload == b'bad-replies':
                    variants = [b'\0\0\1' + wire[3:], b'\0\1' + wire[2:],
                                wire[:3] + b'\3' + wire[4:],
                                wire[:4] + socket.inet_aton('9.9.9.9') + wire[8:],
                                wire[:8] + b'\0\x36' + wire[10:], wire[:10] + bytes(1463)]
                    for bad in variants:
                        relay.sendto(bad, source)
                delay = self.response_delay
                if wire[8:10] == b'\0\x35' and self.dns_response_delay is not None:
                    delay = self.dns_response_delay
                delayed.append((time.monotonic() + delay, relay, source, wire))
                if payload == b'close-control':
                    def close_control(c=entry['tcp']):
                        try:
                            c.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                    timer = threading.Timer(.1, close_control)
                    timer.start()
            now = time.monotonic()
            keep = []
            for deadline, relay, source, wire in delayed:
                if deadline > now:
                    keep.append((deadline, relay, source, wire))
                else:
                    try:
                        relay.sendto(wire, source)
                    except OSError:
                        pass
            delayed = keep

    def close(self):
        self.stop.set()
        self.accept_thread.join(2)
        self.udp_thread.join(2)
        for worker in self.workers:
            worker.join(2)
        self.listener.close()
        if self.common:
            self.common.close()


class UDPRelayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = ROOT / 'tests/.out/udp_relay'
        cls.binary.parent.mkdir(exist_ok=True)
        subprocess.run(['gcc', '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                        '-DUDP_HANDSHAKE_MS=800', '-DUDP_PENDING_MS=300',
                        '-DUDP_IDLE_MS=5000', '-DUDP_DNS_IDLE_MS=1000',
                        '-DUDP_DNS_PRESSURE_MS=1000', '-pthread',
                        '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'main'),
                        str(ROOT / 'tests/udp_relay_harness.c'), str(ROOT / 'main/udp_relay.c'),
                        str(ROOT / 'main/socks_udp.c'), str(ROOT / 'main/dns_flow.c'),
                        str(ROOT / 'main/udp_delivery.c'),
                        str(ROOT / 'main/ap_clients.c'),
                        str(ROOT / 'tests/udp_send_shim.c'), '-Wl,--wrap=send',
                        '-o', str(cls.binary)], check=True)

    @contextlib.contextmanager
    def run_relay(self, env=None, **options):
        peer = Peer(**options)
        proc = subprocess.Popen([str(self.binary), '127.0.0.1', str(peer.port)],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, env={**os.environ, **(env or {})})
        self.clients = []
        self.logs = ''
        try:
            port = int(proc.stdout.readline())
            yield peer, proc, port
        finally:
            for client in self.clients:
                client.close()
            try:
                proc.stdin.write('q'); proc.stdin.flush()
                _, self.logs = proc.communicate(timeout=3)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                proc.kill(); _, self.logs = proc.communicate(timeout=3)
            peer.close()
            self.assertEqual(peer.errors, [], self.logs)
            self.assertEqual(proc.returncode, 0, self.logs)

    def client(self, port, ip='127.0.0.1', source_port=0):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((ip, source_port))
        s.connect(('127.0.0.1', port))
        s.settimeout(2)
        self.clients.append(s)
        return s

    def send(self, c, payload, dest_port=123):
        wire = socket.inet_aton('1.1.1.1') + struct.pack('!H', dest_port) + payload
        c.send(wire)
        return wire

    def dns(self, ident):
        return struct.pack('!6H', ident, 0x0100, 1, 0, 0, 0)

    def command(self, proc, c):
        proc.stdin.write(c); proc.stdin.flush(); time.sleep(.05)

    def test_local_output_retry_reaches_client_once_without_upstream_resend(self):
        with self.run_relay(env={'UDP_DELIVERY_TEST_RETRY': '1'}) as (peer, _, port):
            c = self.client(port)
            wire = self.send(c, b'locally-rejected-output')
            self.assertEqual(c.recv(2048), wire)
            c.settimeout(.15)
            with self.assertRaises(socket.timeout):
                c.recv(2048)
            self.assertEqual(len(peer.received), 1)
        self.assertEqual(self.logs.count('DIAG 30 1'), 0)  # No allocation failure.
        self.assertEqual(self.logs.count('DIAG 31 1'), 2)  # Two ERR_MEM attempts.
        self.assertEqual(self.logs.count('DIAG 36 1'), 2)  # Two scheduled retries.
        self.assertEqual(self.logs.count('DIAG 37 1'), 1)  # Recovered output.
        self.assertEqual(self.logs.count('DIAG 9 1'), 1)   # Exactly one delivery.
        self.assertNotIn('DIAG 23 ', self.logs)           # No final output drop.

    def test_strict_source_ports_same_destination_and_dynamic_relays(self):
        with self.run_relay(split=True) as (peer, _, port):
            a, b = self.client(port), self.client(port)
            request = self.dns(123)  # Same ID and destination on two client ports.
            self.send(a, request, 53); self.send(b, request, 53)
            expected = socket.inet_aton('1.1.1.1') + b'\0\x35' + request[:2] + b'\x81' + request[3:]
            self.assertEqual(a.recv(2048), expected)
            self.assertEqual(b.recv(2048), expected)
            self.assertEqual(peer.accepted, 2)
            self.assertEqual(len({source for _, source, _ in peer.received}), 2)
            self.assertEqual(len({e['udp'].getsockname()[1] for e in peer.associations}), 2)
            request = self.dns(124)
            for c in (a, b):
                self.send(c, request, 53)
                self.assertEqual(c.recv(2048)[6:8], request[:2])
            self.assertEqual(peer.accepted, 2)  # Same flow retains its bound source port.
            for c in (a, b):
                wire = self.send(c, b'again')
                self.assertEqual(c.recv(2048), wire)
            # Those packets have a different original destination port -> new flows.
            self.assertEqual(peer.accepted, 4)

    def test_shared_legacy_relay_and_zero_bound_address(self):
        with self.run_relay(shared=True, zero_ip=True) as (peer, _, port):
            clients = [self.client(port) for _ in range(3)]
            for n, c in enumerate(clients):
                wire = self.send(c, bytes([n]) * 1462)
                self.assertEqual(c.recv(2048), wire)
            self.assertEqual(peer.accepted, 3)

    def test_completed_dns_reuses_association_then_idle_expires(self):
        # Test build scales the firmware's 30s completed-DNS TTL to 1s.
        with self.run_relay() as (peer, _, port):
            c = self.client(port)
            self.send(c, self.dns(1), 53); self.assertEqual(c.recv(2048)[6:8], b'\0\1')
            time.sleep(.6)
            self.send(c, self.dns(2), 53); self.assertEqual(c.recv(2048)[6:8], b'\0\2')
            self.assertEqual(peer.accepted, 1)
            time.sleep(1.2)
            self.send(c, self.dns(3), 53); self.assertEqual(c.recv(2048)[6:8], b'\0\3')
            self.assertEqual(peer.accepted, 2)

    def test_unanswered_dns_survives_completed_dns_idle_ttl(self):
        with self.run_relay(response_delay=1.2) as (peer, _, port):
            c = self.client(port)
            self.send(c, self.dns(1), 53)
            self.assertEqual(c.recv(2048)[6:8], b'\0\1')
            self.assertEqual(peer.accepted, 1)

    def test_control_eof_recovers_only_that_flow(self):
        with self.run_relay() as (peer, _, port):
            a, b = self.client(port), self.client(port)
            wire = self.send(a, b'close-control'); self.assertEqual(a.recv(2048), wire)
            wire = self.send(b, b'unaffected'); self.assertEqual(b.recv(2048), wire)
            time.sleep(.15)
            wire = self.send(a, b'reconnected'); self.assertEqual(a.recv(2048), wire)
            wire = self.send(b, b'still-alive'); self.assertEqual(b.recv(2048), wire)
            self.assertEqual(peer.accepted, 3)

    def test_rejected_association_retries_pending_packet(self):
        with self.run_relay(reject=1) as (peer, _, port):
            c = self.client(port)
            wire = self.send(c, b'pending-first-packet')
            self.assertEqual(c.recv(2048), wire)
            self.assertEqual(peer.accepted, 2)

    def test_transient_udp_send_retains_packet_without_duplicate(self):
        with self.run_relay(env={'UDP_TEST_FIRST_SEND_EAGAIN': '1'}) as (peer, _, port):
            c = self.client(port)
            wire = self.send(c, b'retry-EAGAIN')
            self.assertEqual(c.recv(2048), wire)
            self.assertEqual(len(peer.received), 1)
        self.assertNotIn('DIAG 18 ', self.logs)

    def test_bad_frag_address_type_endpoint_and_oversize_replies_are_dropped(self):
        with self.run_relay() as (_, _, port):
            c = self.client(port)
            wire = self.send(c, b'bad-replies')
            self.assertEqual(c.recv(2048), wire)
            c.settimeout(.1)
            with self.assertRaises(socket.timeout):
                c.recv(2048)
        self.assertEqual(self.logs.count('DIAG 20 1'), 6)

    def test_unsent_datagrams_expire_during_handshake(self):
        with self.run_relay(greeting_delay=.45) as (peer, _, port):
            c = self.client(port)
            self.send(c, b'expired-before-ready')
            time.sleep(.6)
            self.assertEqual(peer.received, [])
            wire = self.send(c, b'fresh-after-ready')
            self.assertEqual(c.recv(2048), wire)
            self.assertEqual(len(peer.received), 1)
        self.assertIn('DIAG 27 1', self.logs)

    def test_per_flow_pending_queue_is_bounded(self):
        with self.run_relay(greeting_delay=.08) as (peer, _, port):
            c = self.client(port)
            for n in range(12):
                self.send(c, bytes([n]))
            replies = []
            c.settimeout(.5)
            while True:
                try:
                    replies.append(c.recv(2048)[6:])
                except socket.timeout:
                    break
            self.assertEqual(replies, [bytes([n]) for n in range(4)])
            self.assertEqual(len(peer.received), 4)
        self.assertEqual(self.logs.count('DIAG 26 1'), 8)

    def test_dns_capacity_reserved_and_completed_queries_reclaimed(self):
        with self.run_relay() as (peer, _, port):
            for n in range(28):
                c = self.client(port)
                wire = self.send(c, bytes([n])); self.assertEqual(c.recv(2048), wire)
            refused = self.client(port)
            self.send(refused, b'ordinary-overflow')
            refused.settimeout(.1)
            with self.assertRaises(socket.timeout):
                refused.recv(2048)
            for n in range(8):
                c = self.client(port)
                query = self.dns(n)
                self.send(c, query, 53)
                response = c.recv(2048)
                self.assertEqual(response[6:8], query[:2])
                self.assertTrue(response[8] & 0x80)
            self.assertEqual(peer.accepted, 36)
        self.assertIn('DIAG 25 1', self.logs)
        self.assertIn('DIAG 24 1', self.logs)

    def test_unanswered_dns_pressure_deadline_admits_new_query(self):
        # Fill the ordinary budget, then pin the four DNS slots with replies
        # delayed beyond the pressure deadline. Fresh queries must survive;
        # once all IDs expire, a new DNS flow must get room without evicting
        # the ordinary flow or waiting for the longer normal UDP idle TTL.
        with self.run_relay(dns_response_delay=20) as (peer, _, port):
            ordinary = []
            for n in range(28):
                c = self.client(port); ordinary.append(c)
                wire = self.send(c, bytes([n])); self.assertEqual(c.recv(2048), wire)
            for n in range(4):
                self.send(self.client(port), self.dns(n), 53)
            deadline = time.monotonic() + .5
            while len(peer.received) < 32 and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertEqual(len(peer.received), 32)
            extra = self.client(port)
            self.send(extra, self.dns(77), 53)
            extra.settimeout(.1)
            with self.assertRaises(socket.timeout): extra.recv(2048)
            self.assertEqual(peer.accepted, 32)  # Fresh pending DNS protected.
            time.sleep(1.05)
            peer.dns_response_delay = 0
            self.send(extra, self.dns(77), 53)
            extra.settimeout(2)
            answer = extra.recv(2048)
            self.assertEqual(answer[6:8], b'\0\x4d')
            self.assertTrue(answer[8] & 0x80)
            self.assertEqual(peer.accepted, 33)
            wire = self.send(ordinary[0], b'ordinary-survives')
            self.assertEqual(ordinary[0].recv(2048), wire)
            self.assertEqual(peer.accepted, 33)
        self.assertIn('DIAG 16 1', self.logs)  # Only pre-deadline denial.
        self.assertIn('DIAG 40 1', self.logs)  # Pressure reclaim, separate from completed.

    def test_dns_has_handshake_capacity_during_ordinary_stalls(self):
        with self.run_relay(stall_first=6) as (peer, _, port):
            for _ in range(6):
                self.send(self.client(port), b'stall')
            deadline = time.monotonic() + 1
            while peer.accepted < 6 and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertEqual(peer.accepted, 6)
            c = self.client(port)
            start = time.monotonic()
            self.send(c, self.dns(55), 53)
            self.assertEqual(c.recv(2048)[6:8], b'\0\x37')
            self.assertLess(time.monotonic() - start, .5)

    def test_two_clients_disconnect_drops_old_reply_only(self):
        with self.run_relay(env={'UDP_TEST_CLIENTS':'1'},response_delay=.2) as (_,proc,port):
            a=self.client(port,'127.0.0.2'); b=self.client(port,'127.0.0.3',a.getsockname()[1])
            self.send(a,b'old-a')
            wire=self.send(b,b'keep-b')
            time.sleep(.05); self.command(proc,'a')
            self.assertEqual(b.recv(2048),wire)
            a.settimeout(.3)
            with self.assertRaises(socket.timeout): a.recv(2048)
            self.command(proc,'b')
            fresh=self.send(a,b'new-a'); a.settimeout(2); self.assertEqual(a.recv(2048),fresh)
            wire=self.send(b,b'still-b'); self.assertEqual(b.recv(2048),wire)
    def test_client_generation_drops_old_replies_and_reopens(self):
        with self.run_relay(response_delay=.2) as (_, proc, port):
            c = self.client(port)
            self.send(c, b'old-generation')
            time.sleep(.05)
            self.command(proc, 'r')
            wire = self.send(c, b'new-generation')
            self.assertEqual(c.recv(2048), wire)
            c.settimeout(.2)
            with self.assertRaises(socket.timeout):
                c.recv(2048)
            self.command(proc, 'd')
            self.send(c, b'offline')
            self.assertEqual(proc.stdout.readline().strip(), 'OFFLINE')
            self.command(proc, 'u')
            c.settimeout(2)
            wire = self.send(c, b'online')
            self.assertEqual(c.recv(2048), wire)


if __name__ == '__main__':
    unittest.main()
