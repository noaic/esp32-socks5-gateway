"""Read-only NO AUTH/IPv4 SOCKS5 UDP DNS probe from the development host.

This bypasses the ESP32 and is an upstream comparison, not an AP path test.
"""
import argparse
import secrets
import socket
import struct
import time


def exact(stream, count):
    result = b""
    while len(result) < count:
        data = stream.recv(count - len(result))
        if not data:
            raise EOFError("SOCKS5 control connection closed")
        result += data
    return result


def skip_name(data, offset):
    while offset < len(data):
        size = data[offset]
        offset += 1
        if not size:
            return offset
        if size & 0xc0 == 0xc0:
            if offset >= len(data):
                break
            return offset + 1
        if size > 63 or offset + size > len(data):
            break
        offset += size
    raise ValueError("Truncated DNS name")


def ipv4_answers(data):
    _, _, questions, answers, _, _ = struct.unpack("!6H", data[:12])
    offset = 12
    for _ in range(questions):
        offset = skip_name(data, offset) + 4
    addresses = []
    for _ in range(answers):
        offset = skip_name(data, offset)
        record_type, record_class, _, size = struct.unpack("!HHIH", data[offset:offset+10])
        offset += 10
        if offset + size > len(data):
            raise ValueError("Truncated DNS record")
        if record_type == 1 and record_class == 1 and size == 4:
            addresses.append(socket.inet_ntoa(data[offset:offset+4]))
        offset += size
    return addresses


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.1.2")
    parser.add_argument("--port", type=int, default=7897)
    parser.add_argument("--dns", default="1.1.1.1")
    parser.add_argument("--name", default="example.com")
    parser.add_argument("--timeout", type=float, default=4)
    args = parser.parse_args()
    upstream = socket.inet_ntoa(socket.inet_aton(args.host))
    dns = socket.inet_aton(args.dns)
    labels = args.name.rstrip(".").encode("idna").split(b".")
    if any(not label or len(label)>63 for label in labels) or sum(map(len,labels))+len(labels)>254:
        parser.error("Invalid DNS name")
    with socket.create_connection((upstream, args.port), args.timeout) as control:
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
            relay = upstream, relay[1]
        query_id = secrets.randbits(16)
        query = struct.pack("!6H", query_id, 0x0100, 1, 0, 0, 0)
        query += b"".join(bytes([len(label)])+label for label in labels) + b"\0\0\1\0\1"
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.settimeout(args.timeout)
            udp.connect(relay)
            wire = b"\x00\x00\x00\x01" + dns + b"\x00\x35" + query
            started = time.monotonic()
            udp.send(wire)
            while True:
                udp.settimeout(max(.001, args.timeout - (time.monotonic() - started)))
                reply = udp.recv(2048)
                if len(reply) >= 22 and reply[:10] == wire[:10] and reply[10:12] == query[:2]:
                    _, flags, _, answers, _, _ = struct.unpack("!6H", reply[10:22])
                    if not flags & 0x8000:
                        continue
                    addresses = ipv4_answers(reply[10:])
                    print(f"relay={relay[0]}:{relay[1]} DNS={args.dns}:53 "
                          f"name={args.name} rcode={flags & 15} answers={answers} A={','.join(addresses)} "
                          f"elapsed_ms={(time.monotonic()-started)*1000:.1f}")
                    if flags & 15 or not addresses:
                        raise RuntimeError("DNS reply did not contain a successful A answer")
                    return
                if time.monotonic() - started >= args.timeout:
                    raise TimeoutError("No matching SOCKS5 UDP DNS reply")


if __name__ == "__main__":
    main()
