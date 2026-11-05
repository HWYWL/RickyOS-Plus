"""Old anti-aliased edges start erasing with the black core, on the real reader waveform."""
from pathlib import Path
import re
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
DISPLAY = PORT / 'firmware/rickyos-plus-1.2/components/metalio_board/display'
source = (DISPLAY / 'ricky_native_turn.inc').read_text()


def actual_function(name):
    start = source.index('static void ' + name + '(')
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


text = (DISPLAY / 'waveforms/my_waveform_gl.h').read_text()
text = text[text.index('= {') + 3:text.index('};')]
raw = bytes(int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', text)).ljust(48 * 64, b'\0')

with tempfile.TemporaryDirectory(prefix='plus-edge-erase-') as directory:
    p = Path(directory)
    (p / 'raw.bin').write_bytes(raw)
    (p / 'test.c').write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
''' + actual_function('ricky_reader_lock_text_finish') + '\n' +
        actual_function('ricky_reader_overlap_erase') + '\n' +
        actual_function('ricky_reader_align_edge_erase') + r'''
static int first_of(const unsigned char* b,int pair){int t=0;while(t<48&&!b[t*1024+pair*4])t++;return t;}
static int last_of(const unsigned char* b,int pair){int t=47;while(t>=0&&!b[t*1024+pair*4])t--;return t;}
int main(int argc,char** argv) {
 assert(argc==2);
 unsigned char raw[48*64],before[48*1024]={0},bank[48*1024],again[48*1024];
 FILE* f=fopen(argv[1],"rb");assert(f&&fread(raw,1,sizeof(raw),f)==sizeof(raw));fclose(f);
 for(int tick=0;tick<48;tick++)for(int pair=0;pair<256;pair++) {
  int to=pair>>4,old=pair&15;
  before[tick*1024+pair*4]=(raw[(tick*16+to)*4+old/4]>>(6-2*(old%4)))&3;
 }
 ricky_reader_lock_text_finish(before,48);
 ricky_reader_overlap_erase(before,48,RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES);
 memcpy(bank,before,sizeof(bank));
 ricky_reader_align_edge_erase(bank,48);
 memcpy(again,bank,sizeof(bank));ricky_reader_align_edge_erase(again,48);
 assert(!memcmp(again,bank,sizeof(bank)));                 // idempotent
 const int core=240,coreFirst=first_of(bank,core);
 assert(coreFirst==first_of(before,core) && coreFirst<48); // the core itself does not move
 for(int pair=0;pair<256;pair++){
  const int to=pair>>4,from=pair&15;
  const int b0=first_of(before,pair),b1=last_of(before,pair),a0=first_of(bank,pair),a1=last_of(bank,pair);
  if(to!=15||from==0||from==15||b0==48){                   // everything else byte-identical
   for(int tick=0;tick<48;tick++)assert(bank[tick*1024+pair*4]==before[tick*1024+pair*4]);
   continue;
  }
  // Same pulses and internal HOLD, in order; only the exterior HOLD moved later.
  assert(a1-a0==b1-b0 && a0>=b0);
  for(int i=0;i<=b1-b0;i++)assert(bank[(a0+i)*1024+pair*4]==before[(b0+i)*1024+pair*4]);
  // Starts with the core, or as late as two final HOLDs allow.
  assert(a0==(b0<coreFirst?(coreFirst<=b0+45-b1?coreFirst:b0+45-b1):b0));
  assert(a1<=45);
  printf("gray%02d->white %2d..%2d -> %2d..%2d\n",from,b0,b1,a0,a1);
 }
 printf("core %d\n",coreFirst);
}
''')
    for overlap in (0, 6):
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                        '-fsanitize=address,undefined', '-DRICKYOS_PLUS_READER_EARLY_ERASE=1',
                        f'-DRICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES={overlap}',
                        str(p / 'test.c'), '-o', str(p / 'test')], check=True)
        print(f'overlap={overlap}')
        print(subprocess.check_output([str(p / 'test'), str(p / 'raw.bin')], text=True), end='')
print('Edge erase with core: gray->white starts with black->white (or clamped), pulses/internal HOLD '
      'preserved, all other transitions byte-identical, idempotent (ASan/UBSan)')
