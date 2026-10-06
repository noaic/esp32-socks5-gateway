"""Run on the AP client, not on the development host; no external packages."""
import argparse
import ipaddress
import random
import socket

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--remote", default="198.51.100.7")
parser.add_argument("--port", type=int, default=443)
parser.add_argument("--client-ip", help="Optional source IPv4 belonging to this terminal")
parser.add_argument("--rounds", type=int, default=64)
args = parser.parse_args()
ipaddress.IPv4Address(args.remote)
if not 1 <= args.port <= 65535 or args.rounds < 0:
    parser.error("Invalid port or rounds")

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as client:
    client.settimeout(8)
    if args.client_ip:
        client.bind((args.client_ip, 0))
    client.connect((args.remote, args.port))
    local, remote = client.getsockname(), client.getpeername()
    print(f"Local={local} Remote={remote}", flush=True)
    if ipaddress.IPv4Address(local[0]) not in ipaddress.IPv4Network("192.168.4.0/24"):
        raise RuntimeError("Connection did not originate from the ESP32 AP subnet")
    if remote != (args.remote, args.port):
        raise RuntimeError("Original remote endpoint changed")
    rng = random.Random(20261002)
    total = 0
    for length in [1, 37, 4096, 16384] + [1024] * args.rounds:
        data = bytes(rng.getrandbits(8) for _ in range(length))
        client.sendall(data)
        reply = bytearray()
        while len(reply) < length:
            part = client.recv(length - len(reply))
            if not part:
                raise RuntimeError("Unexpected EOF")
            reply.extend(part)
        if reply != data:
            raise RuntimeError("Echo payload mismatch")
        total += length
    client.shutdown(socket.SHUT_WR)
    if client.recv(1) != b"":
        raise RuntimeError("Expected server FIN/EOF")
    print(f"PASS: {total} bytes echoed exactly; original remote endpoint preserved; FIN/EOF received")
