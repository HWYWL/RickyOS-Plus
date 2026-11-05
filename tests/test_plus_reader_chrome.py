"""Execute real reader submission + waveform: retained chrome, one scan, fail closed."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'firmware/rickyos-plus-1.2'
DISPLAY = PORT / 'components/metalio_board/display'
source = (DISPLAY / 'highlevel.c').read_text()
actual = source[source.index('static void hold_equal_reader_pixels('):
                source.index('enum EpdDrawError epd_hl_update_screen_from_white(')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include "waveforms/my_waveform.h"
static void test_log(const char* tag,const char* format,...){(void)tag;(void)format;}
#define ESP_LOGI(...) test_log(__VA_ARGS__)
enum EpdDrawMode {MODE_GC16=2,MODE_GC16_FAST=MY_MODE_READER_TURN};
enum EpdDrawError {EPD_DRAW_SUCCESS=0,EPD_DRAW_EMPTY_LINE_QUEUE=1};
enum {MODE_PACKING_1PPB_DIFFERENCE=16,W=64,H=24,N=W*H/2};
typedef struct {int x,y,width,height;} EpdRect;
typedef struct {uint8_t *front_fb,*back_fb,*difference_fb;bool* dirty_lines;uint8_t* dirty_columns;const EpdWaveform* waveform;} EpdiyHighlevelState;
static uint8_t front[N],back[N],diff[N*2],cols[W/2],captured[N*2];
static bool lines[H];
static int calls,selected_mode,fail;
static int epd_width(void){return W;} static int epd_height(void){return H;}
static int esp_timer_get_time(void){return 0;}
static EpdRect epd_full_screen(void){return (EpdRect){0,0,W,H};}
static EpdRect epd_difference_image_cropped(const uint8_t* to,const uint8_t* from,EpdRect area,uint8_t* out,bool* rows,uint8_t* columns){
 (void)area;memset(rows,0,H);memset(columns,0,W/2);
 for(int y=0;y<H;++y)for(int x=0;x<W;++x){
  unsigned shift=(x&1)*4;unsigned i=y*W/2+x/2;
  int t=(to[i]>>shift)&15,f=(from[i]>>shift)&15;
  out[y*W+x]=(uint8_t)((t<<4)|f);if(t!=f){rows[y]=true;columns[x/2]|=15<<shift;}
 }
 return epd_full_screen();
}
static enum EpdDrawError epd_draw_base(EpdRect a,const uint8_t* pixels,EpdRect crop,int mode,int temp,bool* rows,uint8_t* columns,const EpdWaveform* waveform){
 (void)temp;assert(waveform==&MY_WAVEFORM);
 assert(a.x==0&&a.y==0&&a.width==W&&a.height==H);
 assert(memcmp(&a,&crop,sizeof(a))==0);++calls;selected_mode=mode&15;
 for(int y=0;y<H;++y)assert(rows[y]);for(int x=0;x<W/2;++x)assert(columns[x]==255);
 memcpy(captured,pixels,sizeof(captured));
 return fail?EPD_DRAW_EMPTY_LINE_QUEUE:EPD_DRAW_SUCCESS;
}
''' + actual + r'''
static int action(const EpdWaveformPhases* p,int phase,int to,int from){return (p->luts[(phase*16+to)*4+from/4]>>(6-2*(from%4)))&3;}
static int inside(int x,int y,EpdRect b){return x>=b.x&&x<b.x+b.width&&y>=b.y&&y<b.y+b.height;}
int main(void){
 my_waveform_init();const EpdWaveformPhases* turn=NULL;
 for(int i=0;i<MY_WAVEFORM.num_modes;++i)if(MY_WAVEFORM.mode_data[i]->type==MY_MODE_READER_TURN)turn=MY_WAVEFORM.mode_data[i]->range_data[0];
 assert(turn);int retained_black_drive=0;
 for(int f=0;f<turn->phases;++f){assert(action(turn,f,15,15)==0);retained_black_drive+=action(turn,f,0,0)!=0;}
 assert(retained_black_drive==10); // Existing faint-ink protection is not removed.
 EpdiyHighlevelState s={front,back,diff,lines,cols,&MY_WAVEFORM};
 const EpdRect bodies[]={{8,4,48,16},{4,8,52,8},{1,1,62,22}};
 for(int paper=0;paper<2;++paper)for(unsigned b=0;b<sizeof(bodies)/sizeof(bodies[0]);++b){
  for(int value=0;value<256;++value){
   memset(front,(value>>4)*17,N);memset(back,(value&15)*17,N);
   int before=calls;
   assert((paper?epd_hl_update_screen_reader_paper(&s,25,bodies[b]):
                 epd_hl_update_screen_reader(&s,25,bodies[b]))==EPD_DRAW_SUCCESS);
   assert(calls==before+1&&selected_mode==(paper?MY_MODE_READER_PAPER_TURN:MY_MODE_READER_TURN));
   for(int y=0;y<H;++y)for(int x=0;x<W;++x){
    bool equal=(value>>4)==(value&15);bool held=equal&&!inside(x,y,bodies[b]);
    assert(captured[y*W+x]==(held?255:value));
    if(held)for(int f=0;f<turn->phases;++f)assert(action(turn,f,15,15)==0);
   }
   assert(memcmp(front,back,N)==0); // Publish real pixels, not white mask placeholders.
  }
 }
 memset(front,0,N);memset(back,0,N);int before=calls;
 assert(epd_hl_update_screen_full(&s,MODE_GC16,25)==EPD_DRAW_SUCCESS);
 assert(calls==before+1&&selected_mode==MODE_GC16);
 for(int i=0;i<N*2;++i)assert(captured[i]==0); // Cleanup never masks chrome.
 const EpdRect invalid[]={{-1,0,W,H},{0,0,0,0},{0,0,INT_MAX,H},{0,0,W,H+1}};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i){
  assert(epd_hl_update_screen_reader(&s,25,invalid[i])==EPD_DRAW_SUCCESS);
  for(int p=0;p<N*2;++p)assert(captured[p]==0); // Fail safe to the existing full reader scan.
  assert(epd_hl_update_screen_reader_paper(&s,25,invalid[i])==EPD_DRAW_SUCCESS);
  assert(selected_mode==MY_MODE_READER_PAPER_TURN);
  for(int p=0;p<N*2;++p)assert(captured[p]==0);
 }
 memset(front,0,N);memset(back,255,N);fail=1;
 assert(epd_hl_update_screen_reader(&s,25,bodies[0])!=EPD_DRAW_SUCCESS);
 for(int i=0;i<N;++i)assert(back[i]==255); // Failed physical drive never publishes new baseline.
 assert(epd_hl_update_screen_reader_paper(&s,25,bodies[0])!=EPD_DRAW_SUCCESS);
 for(int i=0;i<N;++i)assert(back[i]==255);
}
'''
with tempfile.TemporaryDirectory(prefix='plus-reader-chrome-') as folder:
    work = Path(folder)
    (work / 'test.c').write_text(program)
    (work / 'esp_log.h').write_text('#define ESP_LOGD(...) ((void)0)\n')
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(work), '-I', str(DISPLAY), str(work / 'test.c'),
                    str(DISPLAY / 'waveforms/my_waveform.c'), str(DISPLAY / 'waveforms/my_waveform_trim.c'),
                    '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('Actual reader drive and waveform: 256 transitions x 3 layouts, one scan, unchanged chrome held, body re-drive, full cleanup and failure baseline passed (ASan/UBSan)')
