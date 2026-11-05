"""Native16 must consume the standby/overlay cleanup request only on success."""
from pathlib import Path
import subprocess,tempfile
PORT=Path(__file__).resolve().parents[1]
s=(PORT/'firmware/rickyos-plus-1.2/upstream/lib/GfxRenderer/GfxRenderer.cpp').read_text()
a=s.index('bool GfxRenderer::commitGrayscale16() const {');b=s.index('\nbool GfxRenderer::beginRetainedGrayscale16()',a)
actual=s[a:b]
with tempfile.TemporaryDirectory(prefix='plus-native-cleanup-')as d:
 p=Path(d)
 (p/'test.cpp').write_text(r"""
#include <cassert>
#include <cstdint>
struct Display{bool success=true;int profile=1,commits=0,cancels=0,prepared=0;
 void setNextGray16Profile(int p){assert(p==2);profile=p;++prepared;}};
static bool commitNativeGray(Display&d){++d.commits;return d.success;}
static void cancelNativeGray(Display&d){++d.cancels;}
struct GfxRenderer{
 mutable uint8_t* grayscale16Buffer=nullptr;mutable Display display;
 mutable bool nextRefreshOverridePending=false;mutable int nextRefreshOverride=0;
 bool nextRefreshNeedsCleanup()const{return nextRefreshOverridePending&&nextRefreshOverride!=0;}
 void clearNextRefreshOverride()const{nextRefreshOverridePending=false;}
 bool commitGrayscale16()const;
};
"""+actual+r"""
int main(){
 uint8_t byte=0;
 for(int profile:{1,4,5})for(int requested:{0,1,2})for(bool success:{false,true}){
  GfxRenderer r;r.display.profile=profile;r.display.success=success;
  r.nextRefreshOverridePending=true;r.nextRefreshOverride=requested;
  assert(!r.commitGrayscale16());assert(r.nextRefreshOverridePending&&r.display.commits==0&&r.display.prepared==0);
  r.grayscale16Buffer=&byte;
  assert(r.commitGrayscale16()==success);assert(!r.grayscale16Buffer);
#if FREEINK_DEVICE_METALIO_PLUS
  assert(r.display.profile==(requested?2:profile));assert(r.display.prepared==(requested?1:0));
  assert(r.nextRefreshOverridePending==!success);
#else
  assert(r.display.profile==profile&&r.display.prepared==0&&r.nextRefreshOverridePending);
#endif
  assert(r.display.commits==1&&r.display.cancels==!success);
 }
 GfxRenderer normal;normal.grayscale16Buffer=&byte;assert(normal.commitGrayscale16());assert(normal.display.profile==1&&normal.display.prepared==0);
}
""".replace('#include <cassert>','#include <cassert>'))
 # initializer-list loops require the standard declaration explicitly.
 text=(p/'test.cpp').read_text();(p/'test.cpp').write_text('#include <initializer_list>\n'+text)
 for plus in [0,1]:
  subprocess.run(['clang++','-std=c++20',f'-DFREEINK_DEVICE_METALIO_PLUS={plus}','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
print('Native16 cleanup: standby/full/half override before commit; success consumes; failure retains for BW retry; FAST normal; empty commit and other devices unchanged (ASan/UBSan) PASS')
