import importlib.util,pathlib,threading,time,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('bench_peer',ROOT/'tools/test_stream_server.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class PairTests(unittest.TestCase):
    def test_phase_waits_for_both_and_shares_pair_id(self):
        b=module.PairBarrier(timeout=1,delay=.02); out={}
        def arrive(client):out[client]=b.wait(client,client+'-run','join')
        a=threading.Thread(target=arrive,args=('A',));a.start();time.sleep(.02);self.assertNotIn('A',out)
        arrive('B');a.join(1);self.assertEqual(out['A']['pair_id'],out['B']['pair_id'])
        pair=out['A']['pair_id'];results={}
        def stage(client):results[client]=b.wait(client,client+'-run','1-down',pair)
        a=threading.Thread(target=stage,args=('A',));a.start();time.sleep(.02);self.assertNotIn('A',results)
        stage('B');a.join(1);self.assertEqual(results['A']['pair_id'],pair)
    def test_timeout_and_duplicate_identity(self):
        b=module.PairBarrier(timeout=.02,delay=0)
        with self.assertRaises(ValueError):b.wait('A','a','join')
        with self.assertRaises(ValueError):b.wait('B','b','1-down','wrong')
        self.assertTrue(b.group['closed'])
if __name__=='__main__':unittest.main()
