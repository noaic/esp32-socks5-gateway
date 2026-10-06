"""QUIC TLS handshake through NO AUTH IPv4 SOCKS5 UDP, bypassing ESP32.

Requires aioquic in a separate host Python environment. This verifies the
upstream UDP path, not ESP32 forwarding or full HTTP/3 application traffic.
"""
import argparse
import select
import socket
import struct
import time

from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection
from aioquic.quic.events import ConnectionTerminated, HandshakeCompleted
from probe_upstream_udp import exact


def probe(args):
    destination = (args.ip, 443)
    prefix = b"\0\0\0\1" + socket.inet_aton(args.ip) + struct.pack("!H", 443)
    tx = rx = ignored = 0
    started = time.monotonic()
    with socket.create_connection((args.host, args.port), args.timeout) as control:
        control.settimeout(args.timeout)
        control.sendall(b"\x05\x01\x00")
        if exact(control, 2) != b"\x05\x00":
            raise RuntimeError("NO AUTH rejected")
        control.sendall(b"\x05\x03\x00\x01" + bytes(6))
        header = exact(control, 4)
        if header != b"\x05\x00\x00\x01":
            raise RuntimeError(f"UDP ASSOCIATE reply={header.hex()}")
        bound = exact(control, 6)
        relay = socket.inet_ntoa(bound[:4]), struct.unpack("!H", bound[4:])[0]
        if relay[0] == "0.0.0.0":
            relay = args.host, relay[1]
        if not relay[1]:
            raise RuntimeError("UDP relay advertised port zero")
        config = QuicConfiguration(is_client=True, alpn_protocols=["h3"],
                                   server_name=args.name, max_datagram_size=1200)
        quic = QuicConnection(configuration=config)
        quic.connect(destination, now=time.monotonic())
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.connect(relay)
            udp.setblocking(False)
            deadline = time.monotonic() + args.timeout

            def flush():
                nonlocal tx
                for data, address in quic.datagrams_to_send(now=time.monotonic()):
                    if address != destination:
                        raise RuntimeError("Unexpected QUIC migration destination")
                    udp.send(prefix + data)
                    tx += 1

            while time.monotonic() < deadline:
                flush()
                timer = quic.get_timer()
                end = min(deadline, timer) if timer is not None else deadline
                ready, _, _ = select.select([udp, control], [], [],
                                            max(0, end - time.monotonic()))
                if control in ready:
                    data = control.recv(1)
                    raise RuntimeError("UDP control closed" if not data else
                                       "Unexpected data on UDP control")
                if udp in ready:
                    wire = udp.recv(65535)
                    if len(wire) <= 10 or wire[:10] != prefix:
                        ignored += 1
                        continue
                    rx += 1
                    quic.receive_datagram(wire[10:], destination, now=time.monotonic())
                timer = quic.get_timer()
                if timer is not None and timer <= time.monotonic():
                    quic.handle_timer(now=time.monotonic())
                while (event := quic.next_event()) is not None:
                    if isinstance(event, HandshakeCompleted):
                        quic.close()
                        flush()
                        print(f"QUIC_OK target={args.name}/{args.ip}:443 "
                              f"relay={relay[0]}:{relay[1]} alpn={event.alpn_protocol} "
                              f"tx={tx} rx={rx} ignored={ignored} "
                              f"elapsed_ms={(time.monotonic()-started)*1000:.1f}")
                        return
                    if isinstance(event, ConnectionTerminated):
                        raise RuntimeError(f"QUIC terminated: {event}")
            raise TimeoutError(f"QUIC timeout target={args.name}/{args.ip}:443 "
                               f"tx={tx} rx={rx} ignored={ignored}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.1.2")
    parser.add_argument("--port", type=int, default=7897)
    parser.add_argument("--ip", default="1.1.1.1")
    parser.add_argument("--name", default="cloudflare-dns.com")
    parser.add_argument("--timeout", type=float, default=8)
    args = parser.parse_args()
    socket.inet_aton(args.host)
    socket.inet_aton(args.ip)
    if args.timeout <= 0 or not 0 < args.port < 65536:
        parser.error("timeout and port must be positive and port <= 65535")
    probe(args)
