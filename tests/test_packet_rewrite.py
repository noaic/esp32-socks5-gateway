"""Host tests against the actual portable C rewrite implementation."""
import ctypes
import ipaddress
from pathlib import Path
import random
import struct
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests/.out"
OUT.mkdir(exist_ok=True)
subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                str(ROOT / "main/packet_rewrite.c"), "-o", str(OUT / "rewrite.so")], check=True)
LIB = ctypes.CDLL(str(OUT / "rewrite.so"))


class Packet(ctypes.Structure):
    _fields_ = [("src_ip", ctypes.c_uint32), ("dst_ip", ctypes.c_uint32),
                ("src_port", ctypes.c_uint16), ("dst_port", ctypes.c_uint16),
                ("flags", ctypes.c_uint8)]


LIB.proxy_tcp_parse.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(Packet)]
LIB.proxy_tcp_parse.restype = ctypes.c_bool
LIB.proxy_tcp_parse_local_header.argtypes = [ctypes.c_void_p,ctypes.c_size_t,ctypes.c_size_t,ctypes.POINTER(Packet)]
LIB.proxy_tcp_parse_local_header.restype = ctypes.c_bool
LIB.proxy_tcp_rewrite_validated.argtypes = [ctypes.c_void_p,ctypes.c_bool,ctypes.c_uint32,ctypes.c_uint16,ctypes.c_uint16]
LIB.proxy_tcp_rewrite_validated.restype = None
LIB.proxy_tcp_rewrite.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_bool,
                                 ctypes.c_uint32, ctypes.c_uint16]
LIB.proxy_tcp_rewrite.restype = ctypes.c_bool
LIB.proxy_tcp_rewrite_peer.argtypes = LIB.proxy_tcp_rewrite.argtypes + [ctypes.c_uint16]
LIB.proxy_tcp_rewrite_peer.restype = ctypes.c_bool


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    value = sum(struct.unpack("!" + "H" * (len(data) // 2), data))
    while value > 0xffff:
        value = (value & 0xffff) + (value >> 16)
    return ~value & 0xffff


def address(ip):
    return ipaddress.IPv4Address(ip).packed


def pseudo(ip, tcp):
    return ip[12:20] + struct.pack("!BBH", 0, 6, len(tcp)) + tcp


def packet(payload=b"", options=b"", flags=0x18):
    tcp = bytearray(struct.pack("!HHIIBBHHH", 50000, 443, 0x12345678, 0x98765432,
                                (5 + len(options) // 4) << 4, flags, 12345, 0, 0) + options + payload)
    ip = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0x2c, 20 + len(tcp), 1234,
                               0x4000, 64, 6, 0, address("192.168.4.2"), address("198.51.100.7")))
    ip[10:12] = struct.pack("!H", checksum(ip))
    tcp[16:18] = struct.pack("!H", checksum(pseudo(ip, tcp)))
    return bytes(ip + tcp)


def ip_checksum(data):
    data[10:12] = b"\0\0"
    data[10:12] = struct.pack("!H", checksum(data[:20]))


def parsed(data):
    value = Packet()
    buf = ctypes.create_string_buffer(bytes(data), max(1, len(data)))
    return LIB.proxy_tcp_parse(buf, len(data), ctypes.byref(value)), value


class RewriteTests(unittest.TestCase):
    def test_local_output_header_only_and_incremental_rewrite(self):
        # Pass an actual 40-byte allocation for a large packet: header parsing
        # must not read options/payload that belong to later pbuf segments.
        for length in (0,1,1440,32767):
            data=packet(bytes(range(256))*(length//256)+bytes(length%256),options=b"\x01"*8)
            header=ctypes.create_string_buffer(data[:40],40); view=Packet()
            self.assertTrue(LIB.proxy_tcp_parse_local_header(header,40,len(data),ctypes.byref(view)))
            self.assertFalse(LIB.proxy_tcp_parse_local_header(header,39,len(data),ctypes.byref(view)))
            self.assertFalse(LIB.proxy_tcp_parse_local_header(header,40,len(data)+1,ctypes.byref(view)))
            full=ctypes.create_string_buffer(data,len(data))
            LIB.proxy_tcp_rewrite_validated(full,True,int.from_bytes(address("203.0.113.21"),sys.byteorder),8443,50001)
            self.assertTrue(parsed(full.raw)[0]); self.assertEqual(full.raw[40:],data[40:])
            self.assertEqual((parsed(full.raw)[1].src_port,parsed(full.raw)[1].dst_port),(8443,50001))
        corrupted=bytearray(packet(b"payload")); corrupted[-1]^=1
        header=ctypes.create_string_buffer(bytes(corrupted[:40]),40)
        # The trusted-output parser deliberately does not check TCP payload.
        self.assertTrue(LIB.proxy_tcp_parse_local_header(header,40,len(corrupted),ctypes.byref(view)))
        self.assertFalse(parsed(corrupted)[0]) # Untrusted input still rejects it.

    def test_bidirectional_and_payload_preservation(self):
        rng = random.Random(20261002)
        for length in [0, 1, 2, 3, 37, 511, 1440, 32767, 65495] + [rng.randrange(1460) for _ in range(300)]:
            payload = rng.randbytes(length)
            original = packet(payload)
            ok, view = parsed(original)
            self.assertTrue(ok)
            self.assertEqual((view.src_port, view.dst_port), (50000, 443))
            for source, target, port in [(False, "192.168.4.1", 15000), (True, "203.0.113.21", 8443)]:
                buf = ctypes.create_string_buffer(original, len(original))
                raw_ip = int.from_bytes(address(target), sys.byteorder)
                self.assertTrue(LIB.proxy_tcp_rewrite(buf, len(original), source, raw_ip, port))
                result = buf.raw
                self.assertEqual(checksum(result[:20]), 0)
                self.assertEqual(checksum(pseudo(result[:20], result[20:])), 0)
                self.assertEqual(result[40:], payload)
                expected = bytearray(original)
                expected[12 if source else 16:16 if source else 20] = address(target)
                expected[20 if source else 22:22 if source else 24] = struct.pack("!H", port)
                expected[10:12] = result[10:12]
                expected[36:38] = result[36:38]
                self.assertEqual(result, expected)

    def test_syn_options_fin_rst_and_odd_payload(self):
        options = b"\x02\x04\x05\xa0\x01\x01\x04\x02"
        for flags in [0x02, 0x12, 0x10, 0x11, 0x04, 0x14]:
            data = packet(b"abc", options, flags)
            self.assertTrue(parsed(data)[0])
            buf = ctypes.create_string_buffer(data, len(data))
            self.assertTrue(LIB.proxy_tcp_rewrite(buf, len(data), False,
                            int.from_bytes(address("192.168.4.1"), sys.byteorder), 15000))
            self.assertEqual(buf.raw[32:36], data[32:36])
            self.assertEqual(buf.raw[40:], data[40:])

    def test_internal_peer_tag_and_reverse_endpoint_restoration(self):
        for tag in (20000,20001,20511):
            for flags in (2,0x12,0x10,0x11,0x14):
                original=packet(b"odd-payload",flags=flags)
                buf=ctypes.create_string_buffer(original,len(original))
                self.assertTrue(LIB.proxy_tcp_rewrite_peer(buf,len(original),False,
                    int.from_bytes(address("192.168.4.1"),sys.byteorder),15000,tag))
                wire=buf.raw; ok,view=parsed(wire)
                self.assertTrue(ok); self.assertEqual((view.src_port,view.dst_port),(tag,15000))
                self.assertEqual(wire[24:36],original[24:36]); self.assertEqual(wire[40:],original[40:])
                # Construct the local stack's reply; reverse rewrite restores both visible ports.
                reply=bytearray(wire)
                reply[12:16],reply[16:20]=wire[16:20],wire[12:16]
                reply[20:22],reply[22:24]=wire[22:24],wire[20:22]
                ip_checksum(reply); reply[36:38]=b"\0\0"
                reply[36:38]=struct.pack("!H",checksum(pseudo(reply[:20],reply[20:])))
                back=ctypes.create_string_buffer(bytes(reply),len(reply))
                self.assertTrue(LIB.proxy_tcp_rewrite_peer(back,len(reply),True,
                    int.from_bytes(address("198.51.100.7"),sys.byteorder),443,50000))
                result=back.raw; ok,view=parsed(result)
                self.assertTrue(ok); self.assertEqual((view.src_port,view.dst_port),(443,50000))
                self.assertEqual(result[12:20],address("198.51.100.7")+address("192.168.4.2"))
                self.assertEqual(result[24:36],reply[24:36]); self.assertEqual(result[40:],original[40:])

    def test_incremental_checksum_zero_boundary(self):
        original=bytearray(packet(b"\0\0"))
        expected=bytearray(original)
        expected[16:20]=address("192.168.4.1")
        expected[20:24]=struct.pack("!HH",20000,15000)
        expected[36:38]=b"\0\0"
        word=checksum(pseudo(expected[:20],expected[20:]))
        original[-2:]=struct.pack("!H",word)
        original[36:38]=b"\0\0"
        original[36:38]=struct.pack("!H",checksum(pseudo(original[:20],original[20:])))
        buf=ctypes.create_string_buffer(bytes(original),len(original))
        self.assertTrue(LIB.proxy_tcp_rewrite_peer(buf,len(original),False,
            int.from_bytes(address("192.168.4.1"),sys.byteorder),15000,20000))
        self.assertEqual(buf.raw[36:38],b"\0\0")
        self.assertTrue(parsed(buf.raw)[0])

    def test_reject_corruption_and_truncation_without_mutation(self):
        valid = packet(b"payload")
        candidates = [valid[:n] for n in range(len(valid))]
        for index in [0, 4, 8, 10, 12, 20, 24, 36, 40, 46]:
            broken = bytearray(valid)
            broken[index] ^= 1
            candidates.append(bytes(broken))
        candidates.append(valid + b"extra")
        for broken in candidates:
            self.assertFalse(parsed(broken)[0])
            buf = ctypes.create_string_buffer(broken, max(1, len(broken)))
            before = buf.raw
            self.assertFalse(LIB.proxy_tcp_rewrite(buf, len(broken), False, 0, 15000))
            self.assertEqual(buf.raw, before)

    def test_reject_fragment_options_and_invalid_tcp_header(self):
        for offset in [0x2000, 0x0001, 0x8000, 0x3fff]:
            data = bytearray(packet())
            data[6:8] = struct.pack("!H", offset)
            ip_checksum(data)
            self.assertFalse(parsed(data)[0])
        for header_byte in [0x44, 0x46, 0x65]:
            data = bytearray(packet())
            data[0] = header_byte
            ip_checksum(data)
            self.assertFalse(parsed(data)[0])
        for tcp_offset in [0x40, 0xf0]:
            data = bytearray(packet())
            data[32] = tcp_offset
            data[36:38] = b"\0\0"
            data[36:38] = struct.pack("!H", checksum(pseudo(data[:20], data[20:])))
            self.assertFalse(parsed(data)[0])


if __name__ == "__main__":
    unittest.main()
