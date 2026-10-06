"""Run the actual bounded probe against a local fake SOCKS5/DNS peer."""
import ctypes
import errno
import pathlib
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class Result(ctypes.Structure):
    _fields_=[('code',ctypes.c_int*3),('elapsed_ms',ctypes.c_uint32*3)]
def exact(s,n):
    data=b''
    while len(data)<n:
        p=s.recv(n-len(data))
        if not p:raise EOFError
        data+=p
    return data
class Peer:
    def __init__(self,method=0,rep=0,dns='ok',fragment=False):
        self.method=method;self.rep=rep;self.dns=dns;self.fragment=fragment
        self.errors=[];self.query=None
        self.tcp=socket.socket();self.tcp.bind(('127.0.0.1',0));self.tcp.listen();self.port=self.tcp.getsockname()[1]
        self.udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);self.udp.bind(('127.0.0.1',0));self.udp.settimeout(5)
        self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()
    def send(self,s,data):
        if self.fragment:
            for b in data:s.sendall(bytes([b]));time.sleep(.003)
        else:s.sendall(data)
    def run(self):
        try:
            with self.tcp.accept()[0] as s:
                s.settimeout(6);assert exact(s,3)==b'\x05\x01\x00'
                self.send(s,bytes([5,self.method]))
                if self.method:return
                request=exact(s,10);assert request[:8]==b'\x05\x03\x00\x01'+b'\0'*4
                reply=bytes([5,self.rep,0,1])+socket.inet_aton('127.0.0.1')+struct.pack('!H',self.udp.getsockname()[1]);self.send(s,reply)
                if self.rep:return
                wire,peer=self.udp.recvfrom(512);assert wire[:10]==b'\0\0\0\x01'+socket.inet_aton('1.1.1.1')+b'\0\x35'
                self.query=wire[10:];assert self.query[12:]==b'\x07example\x03com\0\0\x01\0\x01'
                answer=bytearray(self.query);answer[2]=0x81;answer[3]=0x80;answer[7]=1
                answer+=b'\xc0\x0c\0\x01\0\x01\0\0\0\x3c\0\x04'+socket.inet_aton('93.184.216.34')
                if self.dns=='rcode':answer[3]|=3
                if self.dns=='wrong_id':answer[0]^=1
                self.udp.sendto(wire[:10]+answer,peer)
                assert s.recv(1)==b''  # Probe keeps control alive through UDP exchange, then closes it.
        except Exception as e:self.errors.append(e)
    def close(self):
        self.thread.join(6);self.tcp.close();self.udp.close()
        if self.thread.is_alive():raise AssertionError('peer did not finish')
        if self.errors:raise self.errors[0]
class ProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();out=pathlib.Path(cls.tmp.name)/'probe.so'
        subprocess.run(['gcc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-shared','-fPIC','-I',str(ROOT/'main'),str(ROOT/'main/gateway_probe.c'),str(ROOT/'main/socks_udp.c'),'-o',str(out)],check=True)
        cls.lib=ctypes.CDLL(str(out));cls.lib.gateway_probe_run.argtypes=[ctypes.c_char_p,ctypes.c_uint16,ctypes.c_char_p,ctypes.POINTER(Result),ctypes.c_void_p]
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def run_probe(self,**opts):
        peer=Peer(**opts)
        try:
            result=Result();start=time.monotonic();self.lib.gateway_probe_run(b'127.0.0.1',peer.port,b'1.1.1.1',ctypes.byref(result),None)
            return list(result.code),time.monotonic()-start
        finally:peer.close()
    def test_fragmented_handshake_and_dns_over_association(self):self.assertEqual(self.run_probe(fragment=True)[0],[0,0,0])
    def test_noauth_rejection(self):self.assertEqual(self.run_probe(method=255)[0],[0,2255,5000])
    def test_udp_command_rejection(self):self.assertEqual(self.run_probe(rep=7)[0],[0,0,1007])
    def test_dns_error_is_not_success(self):self.assertEqual(self.run_probe(dns='rcode')[0],[0,0,4003])
    def test_unrelated_dns_reply_ignored_and_bounded(self):
        code,elapsed=self.run_probe(dns='wrong_id');self.assertEqual(code,[0,0,-errno.ETIMEDOUT]);self.assertGreater(elapsed,3.8);self.assertLess(elapsed,5)
    def test_invalid_input_skips_later_stages(self):
        result=Result();self.lib.gateway_probe_run(b'not-an-ip',7897,b'1.1.1.1',ctypes.byref(result),None);self.assertEqual(list(result.code),[-errno.EINVAL,5000,5000])
