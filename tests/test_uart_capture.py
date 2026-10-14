"""Actual capture tool on a pseudo UART: one connection, commands and reboot banner."""
import os,pathlib,pty,select,subprocess,tempfile,time,unittest,sys,importlib.util
ROOT=pathlib.Path(__file__).resolve().parents[1]
PYTHON=pathlib.Path(sys.executable)
@unittest.skipUnless(importlib.util.find_spec('serial') and importlib.util.find_spec('esptool'),'pyserial and esptool required')
class CaptureTests(unittest.TestCase):
 def test_timestamp_capture_displays_unterminated_prompt_immediately(self):
  with tempfile.TemporaryDirectory() as d:
   master,slave=pty.openpty();p=subprocess.Popen([str(PYTHON),str(ROOT/'tools/uart_capture.py'),'--port',os.ttyname(slave),'--no-reset','--seconds','1','--log',d+'/log','--timestamps'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
   seen=b''
   try:
    time.sleep(.15);os.write(master,b'gateway> he')
    deadline=time.monotonic()+.6
    while b'gateway> he' not in seen and time.monotonic()<deadline:
     if select.select([p.stdout],[],[],.05)[0]:seen+=os.read(p.stdout.fileno(),2048)
    self.assertIn(b'gateway> he',seen) # No UART newline has been sent.
    out,err=p.communicate(timeout=3);self.assertEqual(p.returncode,0,err.decode())
   finally:
    if p.poll() is None:p.kill();p.communicate()
    os.close(master);os.close(slave)
 def test_passive_capture_same_connection_interactive_and_info_after_banner(self):
  with tempfile.TemporaryDirectory() as d:
   master,slave=pty.openpty();slave_name=os.ttyname(slave)
   p=subprocess.Popen([str(PYTHON),str(ROOT/'tools/uart_capture.py'),'--port',slave_name,'--no-reset','--seconds','5','--log',d+'/log','--interactive','--info-logs'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
   seen=b''
   def read_until(needle,timeout=2):
    nonlocal seen
    end=time.monotonic()+timeout
    while needle not in seen and time.monotonic()<end:
     if select.select([master],[],[],.1)[0]:seen+=os.read(master,1024)
    self.assertIn(needle,seen)
   try:
    time.sleep(.3);os.write(master,b'Ready. Type help. Log: warn.\r\n');read_until(b'log info\n')
    time.sleep(1.2);p.stdin.write(b'status\n');p.stdin.flush();read_until(b'status\n')
    os.write(master,b'uptime_s=10\r\ninternal_free=90000\r\n')
    p.stdin.write(b'x'*110+b'\n');p.stdin.flush()
    time.sleep(.3);seen=b'';os.write(master,b'Ready. Type help. Log: warn.\r\n');read_until(b'log info\n')
    out,err=p.communicate(timeout=7);self.assertEqual(p.returncode,0,err.decode());self.assertIn(b'Invalid command length',out)
    log=pathlib.Path(d+'/log').read_bytes();self.assertIn(b'uptime_s=10',log);self.assertIn(b'internal_free=90000',log)
   finally:
    if p.poll() is None:p.kill();p.communicate()
    os.close(master);os.close(slave)
