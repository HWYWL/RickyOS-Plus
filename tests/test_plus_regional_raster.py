"""Execute vendor raster addressing; regional masks must not shift whole-row source."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
d=ROOT/'firmware/rickyos-plus-1.2/components/metalio_board/display'
s=(d/'output_common/render_context.c').read_text();a=s.index('void get_buffer_params(');b=s.index('\nvoid IRAM_ATTR',a)
code=r'''
#include <cassert>
#include <cstdint>
#include <array>
struct EpdRect {int x,y,width,height;};
enum EpdDrawMode {MODE_PACKING_1PPB_DIFFERENCE=16,MODE_PACKING_2PPB=32,MODE_PACKING_8PPB=64};
constexpr int EPD_DRAW_INVALID_PACKING_MODE=128;
struct RenderContext_t {EpdRect area,crop_to;EpdDrawMode mode;const uint8_t* data_ptr;int error=0;};
int min(int a,int b){return a<b?a:b;}
'''+s[a:b]+r'''
int main(){
 constexpr int W=1216,H=684;
 std::array<uint8_t,W*H> raster{};
 for(int x: {0,32,64,1184})for(int y: {0,16,600}) {
   // Actual regional submission uses full input/crop dimensions; dirty masks
   // carry x/y selection. Every fetched pixel remains at its true scan index.
   RenderContext_t ctx{{0,0,W,H},{0,0,W,H},MODE_PACKING_1PPB_DIFFERENCE,raster.data()};
   int stride,lo,hi,ppb;const uint8_t* start;
   get_buffer_params(&ctx,&stride,&start,&lo,&hi,&ppb);
   assert(stride==W && lo==0 && hi==H && start==raster.data() && ppb==1);
   for(int row: {y,H-1}) {
     const uint8_t* line=start+stride*(row-lo);
     assert(line+x==raster.data()+row*W+x);
     assert(line+W<=raster.data()+raster.size());
   }
   // Reproduce the previous crop bug: a nonzero x shifts whole-row decoding
   // and the final full-row decode extends beyond the interlaced allocation.
   if(x) {
     ctx.crop_to={x,y,W-x,H-y};get_buffer_params(&ctx,&stride,&start,&lo,&hi,&ppb);
     assert(start==raster.data()+y*W+x);
   }
 }
}
'''
high=(d/'highlevel.c').read_text()
assert 'epd_draw_base(epd_full_screen(), state->difference_fb, epd_full_screen(),' in high
with tempfile.TemporaryDirectory(prefix='plus-regional-raster-') as folder:
 p=Path(folder);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual vendor scan addressing passed: nonzero regional x/y, full raster stride, final-row bounds')
