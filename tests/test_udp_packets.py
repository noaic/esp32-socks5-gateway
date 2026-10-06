"""Validate production UDP input checksums and SOCKS5 UDP wire boundaries."""
import ctypes
import ipaddress
import random
import struct
import subprocess
import unittest
import test_packet_rewrite as tcp


class UDPPacket(ctypes.Structure):
    _fields_ = [('src_ip', ctypes.c_uint32), ('dst_ip', ctypes.c_uint32),
                ('src_port', ctypes.c_uint16), ('dst_port', ctypes.c_uint16),
                ('payload_length', ctypes.c_uint16)]


tcp.LIB.proxy_udp_parse.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(UDPPacket)]
tcp.LIB.proxy_udp_parse.restype = ctypes.c_bool
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                str(tcp.ROOT / 'main/socks_udp.c'), '-o', str(tcp.OUT / 'socks_udp.so')], check=True)
CODEC = ctypes.CDLL(str(tcp.OUT / 'socks_udp.so'))
CODEC.socks_udp_encode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32,
                                  ctypes.c_uint16, ctypes.c_void_p, ctypes.c_size_t]
CODEC.socks_udp_encode.restype = ctypes.c_size_t
CODEC.socks_udp_decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint32),
                                  ctypes.POINTER(ctypes.c_uint16), ctypes.POINTER(ctypes.c_void_p),
                                  ctypes.POINTER(ctypes.c_size_t)]
CODEC.socks_udp_decode.restype = ctypes.c_bool


def packet(payload, checksum=True):
    src, dst = tcp.address('192.168.4.2'), tcp.address('1.1.1.1')
    udp = bytearray(struct.pack('!4H', 51000, 53, len(payload) + 8, 0) + payload)
    if checksum:
        value = tcp.checksum(src + dst + struct.pack('!BBH', 0, 17, len(udp)) + udp)
        udp[6:8] = struct.pack('!H', value or 0xffff)
    ip = bytearray(struct.pack('!BBHHHBBH4s4s', 0x45, 0, len(udp) + 20, 100,
                               0x4000, 64, 17, 0, src, dst))
    ip[10:12] = struct.pack('!H', tcp.checksum(ip))
    return ip + udp


def parsed(data):
    result = UDPPacket()
    buf = ctypes.create_string_buffer(bytes(data), max(1, len(data)))
    return tcp.LIB.proxy_udp_parse(buf, len(data), ctypes.byref(result)), result


def decoded(data):
    buf = ctypes.create_string_buffer(bytes(data), max(1, len(data)))
    addr, port, payload, size = ctypes.c_uint32(), ctypes.c_uint16(), ctypes.c_void_p(), ctypes.c_size_t()
    ok = CODEC.socks_udp_decode(buf, len(data), ctypes.byref(addr), ctypes.byref(port),
                               ctypes.byref(payload), ctypes.byref(size))
    return ok, addr.value, port.value, ctypes.string_at(payload, size.value) if ok else b''


class UDPWireTests(unittest.TestCase):
    def test_udp_checksums_lengths_and_payload(self):
        rng = random.Random(20261002)
        for length in [0, 1, 2, 1462, 65507] + [rng.randrange(1463) for _ in range(50)]:
            data = packet(rng.randbytes(length))
            ok, view = parsed(data)
            self.assertTrue(ok)
            self.assertEqual((view.src_port, view.dst_port, view.payload_length), (51000, 53, length))
            if length:
                data[-1] ^= 1
                self.assertFalse(parsed(data)[0])
                data[26:28] = b'\0\0'
                self.assertTrue(parsed(data)[0])  # IPv4 explicitly permits absent UDP checksum.
            data[10] ^= 1
            self.assertFalse(parsed(data)[0])

    def test_udp_rejects_fragments_options_bad_lengths_and_ports(self):
        original = packet(b'odd', checksum=False)
        for offset, value in [(0, 0x46), (9, 6), (6, 0x20), (7, 1),
                              (20, 0), (22, 0), (24, 1)]:
            data = bytearray(original)
            data[offset] = value
            if offset in (20, 22):
                data[offset:offset + 2] = b'\0\0'
            tcp.ip_checksum(data)
            self.assertFalse(parsed(data)[0], (offset, value))
        for n in range(len(original)):
            self.assertFalse(parsed(original[:n])[0])
        self.assertFalse(parsed(original + b'padding')[0])

    def test_socks_udp_roundtrip_and_capacity(self):
        raw_ip = int.from_bytes(tcp.address('203.0.113.7'), tcp.sys.byteorder)
        for length in (0, 1, 1462, 65497):
            data = bytes([length % 256]) * length
            payload = ctypes.create_string_buffer(data, max(1, length))
            out = ctypes.create_string_buffer(length + 10)
            self.assertEqual(CODEC.socks_udp_encode(out, len(out), raw_ip, 65535, payload, length), length + 10)
            self.assertEqual(out.raw[:10], b'\0\0\0\1' + tcp.address('203.0.113.7') + b'\xff\xff')
            self.assertEqual(decoded(out.raw), (True, raw_ip, 65535, data))
            self.assertEqual(CODEC.socks_udp_encode(out, len(out) - 1, raw_ip, 65535, payload, length), 0)
            self.assertEqual(CODEC.socks_udp_encode(out, len(out), raw_ip, 0, payload, length), 0)
        out = ctypes.create_string_buffer(65508)
        self.assertEqual(CODEC.socks_udp_encode(out, len(out), raw_ip, 53, out, 65498), 0)

    def test_socks_udp_rejects_frag_reserved_atyp_and_zero_port(self):
        wire = bytearray(b'\0\0\0\1' + tcp.address('1.1.1.1') + b'\0\x35' + b'payload')
        for n in range(10):
            self.assertFalse(decoded(wire[:n])[0])
        for offset, value in [(0, 1), (1, 1), (2, 1), (2, 255), (3, 3), (3, 4), (9, 0)]:
            invalid = bytearray(wire); invalid[offset] = value
            self.assertFalse(decoded(invalid)[0])
        self.assertFalse(decoded(bytes(65508))[0])


if __name__ == '__main__':
    unittest.main()
