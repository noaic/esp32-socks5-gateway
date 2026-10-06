import pathlib, subprocess, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class ClientTests(unittest.TestCase):
    def test_lifecycle_quotas_and_mapping_isolation(self):
        binary=ROOT/'tests/.out/ap_clients'; binary.parent.mkdir(exist_ok=True)
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-pthread','-I',str(ROOT/'main'),
            str(ROOT/'tests/ap_clients_harness.c'),str(ROOT/'main/ap_clients.c'),str(ROOT/'main/flow_table.c'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
if __name__=='__main__': unittest.main()
