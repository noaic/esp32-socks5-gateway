"""Actual production DNS/HTTP checks against a SOCKS-only protocol fixture."""
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
from test_gateway_probe import Result, exact, ROOT
class InternetResult(ctypes.Structure):
    _fields_=[('network',Result),('http_code',ctypes.c_int),('http_ms',ctypes.c_uint32),('ipv4',ctypes.c_uint32),('http_status',ctypes.c_uint16)]
def name(host):return b''.join(bytes([len(x)])+x.encode() for x in host.split('.'))+b'\0'
def rr(owner,kind,data):return owner+struct.pack('!HHIH',kind,1,60,len(data))+data
class Peer:
    def __init__(self,target=0,dns='a',response=None,fragment=False,rep=0):
        self.target=target;self.dns=dns;self.response=response;self.fragment=fragment;self.rep=rep;self.errors=[];self.http_seen=False
        self.tcp=socket.socket();self.tcp.bind(('127.0.0.1',0));self.tcp.listen();self.tcp.settimeout(6);self.port=self.tcp.getsockname()[1]
        self.udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);self.udp.bind(('127.0.0.1',0));self.udp.settimeout(5)
        self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()
    def send(self,s,p):
        if self.fragment:
            for i in range(0,len(p),3):s.sendall(p[i:i+3]);time.sleep(.002)
        else:s.sendall(p)
    def run(self):
        try:
            host=['www.msftconnecttest.com','connectivitycheck.gstatic.com'][self.target]
            with self.tcp.accept()[0] as s:
                s.settimeout(5);assert exact(s,3)==b'\5\1\0';self.send(s,b'\5\0')
                assert exact(s,10)[:8]==b'\5\3\0\1'+b'\0'*4
                self.send(s,b'\5\0\0\1'+socket.inet_aton('127.0.0.1')+struct.pack('!H',self.udp.getsockname()[1]))
                wire,address=self.udp.recvfrom(512);query=wire[10:];assert query[12:]==name(host)+b'\0\1\0\1'
                data=socket.inet_aton('93.184.216.34');records=rr(b'\xc0\x0c',1,data);count=1
                if self.dns=='private':records=rr(b'\xc0\x0c',1,socket.inet_aton('192.168.4.1'))
                if self.dns=='unrelated':records=rr(name('unrelated.example'),1,data)
                if self.dns=='loop':records=rr(b'\xc0\x0c',5,b'\xc0\x0c')
                if self.dns=='truncated':records=records[:-1]
                if self.dns=='cname':records=rr(name('alias.example'),1,data)+rr(b'\xc0\x0c',5,name('alias.example'));count=2
                answer=bytearray(query);answer[2:4]=b'\x81\x80';answer[6:8]=struct.pack('!H',count)
                self.udp.sendto(wire[:10]+answer+records,address);assert s.recv(1)==b''
            if self.dns not in ('a','cname'):return
            with self.tcp.accept()[0] as s:
                s.settimeout(5);assert exact(s,3)==b'\5\1\0';self.send(s,b'\5\0')
                request=exact(s,10);assert request==b'\5\1\0\1'+data+b'\0\x50';self.http_seen=True
                self.send(s,bytes([5,self.rep,0,1])+b'\0'*6)
                if self.rep:return
                request=b''
                while b'\r\n\r\n' not in request:request+=s.recv(256)
                assert (b'Host: '+host.encode()+b'\r\n') in request
                assert request.startswith(b'GET '+[b'/connecttest.txt',b'/generate_204'][self.target]+b' HTTP/1.0\r\n')
                response=self.response
                if response is None:response=[b'HTTP/1.1 200 OK\r\nContent-Length: 22\r\n\r\nMicrosoft Connect Test',b'HTTP/1.1 204 No Content\r\n\r\n'][self.target]
                if response==b'wait':time.sleep(4.2)
                else:self.send(s,response)
        except (BrokenPipeError,ConnectionResetError):pass  # Checker rejects oversized or malformed replies early.
        except Exception as e:self.errors.append(e)
    def close(self):
        self.thread.join(7);self.tcp.close();self.udp.close()
        if self.thread.is_alive():raise AssertionError('fixture did not stop')
        if self.errors:raise self.errors[0]
class InternetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();out=pathlib.Path(cls.tmp.name)/'check.so'
        subprocess.run(['gcc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-shared','-fPIC','-I',str(ROOT/'main'),str(ROOT/'main/gateway_probe.c'),str(ROOT/'main/socks_udp.c'),'-o',str(out)],check=True)
        cls.lib=ctypes.CDLL(str(out));cls.lib.gateway_internet_run.argtypes=[ctypes.c_char_p,ctypes.c_uint16,ctypes.c_char_p,ctypes.c_uint,ctypes.POINTER(InternetResult),ctypes.c_void_p]
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def check(self,**opts):
        peer=Peer(**opts)
        try:
            r=InternetResult();start=time.monotonic();self.lib.gateway_internet_run(b'127.0.0.1',peer.port,b'1.1.1.1',opts.get('target',0),ctypes.byref(r),None)
            return r,time.monotonic()-start,peer.http_seen
        finally:peer.close()
    def test_fragmented_success_both_targets_and_ipv4_socks_connect(self):
        for target,status in [(0,200),(1,204)]:
            with self.subTest(target=target):
                r,_,seen=self.check(target=target,fragment=True);self.assertEqual(list(r.network.code),[0,0,0]);self.assertEqual(r.http_code,0);self.assertEqual(r.http_status,status);self.assertTrue(seen)
    def test_cname_out_of_order(self):self.assertEqual(self.check(dns='cname')[0].http_code,0)
    def test_private_unrelated_loop_and_truncated_dns_rejected(self):
        for dns in ['private','unrelated','loop','truncated']:
            with self.subTest(dns=dns):
                r,_,seen=self.check(dns=dns);self.assertEqual(r.network.code[2],3000);self.assertEqual(r.http_code,5000);self.assertFalse(seen)
    def test_redirect_is_not_internet_success(self):self.assertEqual(self.check(response=b'HTTP/1.1 302 Found\r\nLocation: http://login/\r\n\r\n')[0].http_code,6302)
    def test_wrong_body_and_length(self):
        for response in [b'HTTP/1.1 200 OK\r\nContent-Length: 22\r\n\r\nCaptive portal message',b'HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\n']:
            self.assertEqual(self.check(response=response)[0].http_code,7000)
    def test_close_delimited_body(self):self.assertEqual(self.check(response=b'HTTP/1.0 200 OK\r\n\r\nMicrosoft Connect Test')[0].http_code,0)
    def test_short_body(self):self.assertEqual(self.check(response=b'HTTP/1.1 200 OK\r\nContent-Length: 22\r\n\r\nMicrosoft')[0].http_code,-errno.ECONNRESET)
    def test_malformed_duplicate_length_transfer_encoding_and_header_bound(self):
        for header in [b'Content-Length: 22\r\nContent-Length: 21',b'Transfer-Encoding: chunked',b'Content-Length: NaN',b'Broken',b'X: '+b'a'*1100]:
            self.assertEqual(self.check(response=b'HTTP/1.1 200 OK\r\n'+header+b'\r\n\r\n')[0].http_code,3000)
    def test_socks_rejection(self):self.assertEqual(self.check(rep=5)[0].http_code,1005)
    def test_deadline_without_http_response(self):
        r,elapsed,_=self.check(response=b'wait');self.assertEqual(r.http_code,-errno.ETIMEDOUT);self.assertGreater(elapsed,3.8);self.assertLess(elapsed,4.5)
    def test_invalid_target_does_not_connect(self):
        r=InternetResult();self.lib.gateway_internet_run(b'127.0.0.1',1,b'1.1.1.1',2,ctypes.byref(r),None);self.assertEqual(r.network.code[0],-errno.EINVAL);self.assertEqual(r.http_code,5000)
