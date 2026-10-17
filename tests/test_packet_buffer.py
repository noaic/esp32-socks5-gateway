"""Exercise the inactive packet-buffer experiment, not the firmware input path."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
API = r'''
#include <stdint.h>
#include <stddef.h>
typedef enum { PBUF_RAW=0 } pbuf_layer;
typedef enum { PBUF_RAM=0x280, PBUF_ROM=1, PBUF_REF=0x41, PBUF_POOL=0x182 } pbuf_type;
#define PBUF_FLAG_IS_CUSTOM 2
struct pbuf { struct pbuf *next; void *payload; uint16_t tot_len,len,ref; uint8_t type_internal,flags; };
struct pbuf *pbuf_clone(pbuf_layer,pbuf_type,struct pbuf *);
uint8_t pbuf_free(struct pbuf *);
'''
HARNESS = r'''
#include "packet_buffer.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static unsigned clones,frees;
static int fail_clone;
static struct pbuf *make(pbuf_type type) {
 struct pbuf *p=calloc(1,sizeof(*p)); assert(p);
 p->payload=malloc(64); assert(p->payload); memset(p->payload,0x5a,64);
 p->len=p->tot_len=64; p->ref=1; p->type_internal=(uint8_t)type; return p;
}
uint8_t pbuf_free(struct pbuf *p) {
 assert(p && p->ref); if(--p->ref) return 0;
 struct pbuf *next=p->next; frees++; free(p->payload); free(p);
 if(next) pbuf_free(next);
 return 1;
}
struct pbuf *pbuf_clone(pbuf_layer layer,pbuf_type type,struct pbuf *p) {
 assert(layer==PBUF_RAW && type==PBUF_RAM); clones++;
 if(fail_clone)return NULL;
 struct pbuf *q=make(type); memcpy(q->payload,p->payload,p->len); return q;
}
int main(void) {
 assert(packet_buffer_prepare(NULL)==NULL);
 for(unsigned i=0;i<10000;i++) {
  struct pbuf *original=make(PBUF_RAM), *prepared=packet_buffer_prepare(original);
  assert(prepared==original && clones==0);
  unsigned n=frees;
  packet_buffer_discard(original,prepared); /* Parser/mapping error: caller owns original. */
  assert(frees==n && original->ref==1 && ((unsigned char*)original->payload)[0]==0x5a);
  ((unsigned char*)prepared->payload)[0]=0x33;
  packet_buffer_commit(&original,prepared);
  assert(original==prepared && frees==n && ((unsigned char*)original->payload)[0]==0x33);
  pbuf_free(original); assert(frees==n+1);
 }
 for(unsigned kind=0;kind<6;kind++) {
  struct pbuf *original=make(kind==0?PBUF_REF:kind==1?PBUF_ROM:kind==2?PBUF_POOL:PBUF_RAM);
  if(kind==3)original->ref=2;
  if(kind==4)original->flags=PBUF_FLAG_IS_CUSTOM;
  if(kind==5){original->next=make(PBUF_RAM);original->len=32;original->next->len=original->next->tot_len=32;}
  unsigned before=clones;
  struct pbuf *prepared=packet_buffer_prepare(original);
  assert(prepared && prepared!=original && clones==before+1);
  ((unsigned char*)prepared->payload)[0]=0x44;
  assert(((unsigned char*)original->payload)[0]==0x5a);
  unsigned n=frees;packet_buffer_discard(original,prepared);assert(frees==n+1);
  assert(original->ref==(kind==3?2:1));
  prepared=packet_buffer_prepare(original);struct pbuf *old=original;
  packet_buffer_commit(&original,prepared);assert(original==prepared);
  if(kind==3){assert(old->ref==1);pbuf_free(old);}
  pbuf_free(original);
 }
 struct pbuf *original=make(PBUF_REF);fail_clone=1;
 struct pbuf *prepared=packet_buffer_prepare(original);assert(prepared==NULL);
 unsigned n=frees;packet_buffer_discard(original,prepared);packet_buffer_commit(&original,prepared);
 assert(frees==n && original->ref==1);pbuf_free(original);
 return 0;
}
'''


class ExperimentalBufferTests(unittest.TestCase):
    def test_private_reuse_fallback_and_error_ownership(self):
        with tempfile.TemporaryDirectory(prefix='esp-packet-buffer-') as temp:
            path = Path(temp)
            (path / 'lwip').mkdir()
            (path / 'lwip/pbuf.h').write_text('#pragma once\n' + API)
            (path / 'harness.c').write_text(HARNESS)
            binary = path / 'check'
            subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I', str(path), '-I', str(ROOT / 'experiments'),
                            str(path / 'harness.c'), str(ROOT / 'experiments/packet_buffer.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
