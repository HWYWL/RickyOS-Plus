"""Execute real SDK facade transactions against the actual Plus panel driver."""
import sys
from test_plus_regional_gray import PORT, shim, high, backend, driver, run_program

sys.path.insert(0, str(PORT / 'upstream/scripts/tests'))
from test_reading_ui_regressions import method

source = (PORT / 'components/rickyos_display/src/FreeInkDisplay.cpp').read_text()
functions = '\n'.join(method(source, name) for name in (
    'GrayscaleCapabilities FreeInkDisplay::grayscaleCapabilities(',
    'void FreeInkDisplay::displayBuffer(',
    'void FreeInkDisplay::cancelGrayscale16(',
    'void FreeInkDisplay::cancelGrayscalePass(',
    'bool FreeInkDisplay::acceptGrayscaleRows(',
    'void FreeInkDisplay::copyGrayscaleLsbBuffers(',
    'void FreeInkDisplay::copyGrayscaleMsbBuffers(',
    'void FreeInkDisplay::copyGrayscaleBuffers(',
    'void FreeInkDisplay::displayGrayBuffer(',
    'void FreeInkDisplay::cleanupGrayscaleBuffers(',
))
backend = backend.replace('struct EpdBus{};', '''
enum class RefreshContext {Generic};
struct EpdBus{};
''').replace(' virtual void cleanupGrayscaleBuffers', '''
 virtual void displayWithContext(EpdBus& bus,const uint8_t* fb,const uint8_t* prev,RefreshMode mode,bool off,RefreshContext){
   display(bus,fb,prev,mode,off);
 }
 virtual void cleanupGrayscaleBuffers''')

facade = r'''
#define EINK_DISPLAY_SINGLE_BUFFER_MODE 1
#define FREEINK_SSD1677_TEXT_ROUTING 0
namespace freeink {
static void invertBytes(uint8_t* data,size_t bytes){for(size_t i=0;i<bytes;++i)data[i]^=255;}
class FreeInkDisplay {
public:
 using RefreshMode=freeink::RefreshMode;
 static constexpr auto FAST_REFRESH=RefreshMode::Fast,HALF_REFRESH=RefreshMode::Half;
 enum class PanelSel {X3,Other};
 PanelSel _panelSel=PanelSel::Other;
 PanelDriver* _driver;
 EpdBus _bus;
 uint8_t* frameBuffer;
 size_t bufferSize=W*H/8;
 bool _inverted=false,_inversionDirty=false,_grayPassFailed=false,_nativeGrayActive=false;
 bool _shadowValid=false,_redRamSynced=false;
 GrayscaleMode _grayscaleMode=GrayscaleMode::Overlay;
 uint16_t _grayRows[2]{};
 FreeInkDisplay(PanelDriver& d,uint8_t* fb):_driver(&d),frameBuffer(fb){}
 uint16_t getDisplayHeight()const{return H;}
 void syncPendingAsync(){}
 static RefreshMode toInternal(RefreshMode mode){return mode;}
 GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode)const;
 void displayBuffer(RefreshMode,bool,RefreshContext);
 void cancelGrayscale16();
 void cancelGrayscalePass();
 bool acceptGrayscaleRows(unsigned,const uint8_t*,uint16_t,uint16_t);
 void copyGrayscaleLsbBuffers(const uint8_t*);
 void copyGrayscaleMsbBuffers(const uint8_t*);
 void copyGrayscaleBuffers(const uint8_t*,const uint8_t*);
 void displayGrayBuffer(bool,const unsigned char*,bool);
 void cleanupGrayscaleBuffers(const uint8_t*);
};
''' + functions + '\n}\n'
main = r'''
int main(){
 using namespace freeink;
 EpdBus bus;auto& d=metalioPlusDriver();d.begin(bus);
 std::vector<uint8_t> bw(W*H/8,255),lsb(bw.size(),0),msb(bw.size(),0),live(bw);
 bw[0]=0x14;lsb[0]=0x41;msb[0]=0x63;live=bw;
 FreeInkDisplay f(d,live.data());
 assert(f.grayscaleCapabilities(GrayscaleMode::Overlay).supported());
 assert(!f.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
 const uint8_t expected[]{0,5,10,15,0,15,10,5};
 for(bool combined:{false,true}){
  live=bw;f.displayBuffer(RefreshMode::Fast,false,RefreshContext::Generic);
  const int before=powered;
  if(combined)f.copyGrayscaleBuffers(lsb.data(),msb.data());
  else{
   live=lsb;f.copyGrayscaleLsbBuffers(live.data());
   live=msb;f.copyGrayscaleMsbBuffers(live.data());
  }
  f.displayGrayBuffer(false,nullptr,false);
  assert(powered==before+1 && d.opticalState()==OpticalState::Gray);
  for(unsigned x=0;x<8;++x)assert(((front[x/2]>>((x&1)*4))&15)==expected[x]);
  for(int i=4;i<N;++i)assert(front[i]==255);
  f.cleanupGrayscaleBuffers(bw.data());
  assert(live==bw && memcmp(front,back,N)==0);
 }
 // Late MSB without LSB leaves the displayed B/W page untouched.
 f.displayBuffer(RefreshMode::Fast,false,RefreshContext::Generic);
 int before=powered;
 live=msb;f.copyGrayscaleMsbBuffers(live.data());f.displayGrayBuffer(false,nullptr,false);
 assert(powered==before && memcmp(front,back,N)==0);
 f.cleanupGrayscaleBuffers(bw.data());assert(live==bw);
 // Night mode still inverts only the final B/W page. Gray calls are ignored,
 // and the application's logical B/W buffer is restored after displayBuffer.
 f._inverted=true;f._inversionDirty=true;
 f.displayBuffer(RefreshMode::Fast,false,RefreshContext::Generic);
 assert(live==bw && !f.grayscaleCapabilities(GrayscaleMode::Overlay).supported());
 for(unsigned x=0;x<8;++x)assert(((front[x/2]>>((x&1)*4))&15)==((bw[0]&(0x80u>>x))?0:15));
 before=powered;
 f.copyGrayscaleBuffers(lsb.data(),msb.data());f.displayGrayBuffer(false,nullptr,false);
 assert(powered==before && memcmp(front,back,N)==0);
 // Return to normal, then a native 16-level JPEG/text page. No overlay marker
 // leaks into that transaction and arbitrary intermediate levels survive.
 f._inverted=false;f._inversionDirty=true;
 f.displayBuffer(RefreshMode::Fast,false,RefreshContext::Generic);
 auto* native=d.beginGrayscale16();assert(native && native[0]==255 && native[N-1]==255);
 native[0]=0x87;metalioPlusSetGray16Profile(1);before=powered;
 assert(d.commitGrayscale16(live.data()) && powered==before+1 && front[0]==0x87);
 assert(d.opticalState()==OpticalState::Gray && powered==off);
}
'''
run_program(shim + high + backend + driver + facade + main)
print('Plus actual facade + driver: image/AA selector polarity, both upload APIs, B/W fallback, night mode and native16 transition passed (ASan/UBSan)')
