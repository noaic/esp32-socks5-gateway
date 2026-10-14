"""Production metadata capacity, per-slot admission and safe retirement guards."""
import pathlib,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class FlowTests(unittest.TestCase):
 def test_client_syn_bursts_retained_tokens_and_other_client_capacity(self):
  with tempfile.TemporaryDirectory() as d:
   binary=pathlib.Path(d)/'check'
   subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),
    str(ROOT/'tests/flow_table_harness.c'),str(ROOT/'main/flow_table.c'),'-o',str(binary)],check=True)
   subprocess.run([str(binary)],check=True)
