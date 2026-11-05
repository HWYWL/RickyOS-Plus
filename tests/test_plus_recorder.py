"""Actual recorder HAL and WAV format, with deterministic audio/storage failures."""
from pathlib import Path
import tempfile, subprocess
root=Path(__file__).resolve().parents[1]
hal=root/'firmware/rickyos-plus-1.2/upstream/lib/hal'
with tempfile.TemporaryDirectory() as tmp:
 d=Path(tmp)
 (d/'Logging.h').write_text('#define LOG_INF(...) ((void)0)\n#define LOG_ERR(...) ((void)0)\n')
 (d/'PlusAudio.h').write_text('''#pragma once
#include <cstddef>
#include <cstdint>
namespace plus_audio {
inline bool running=false, error=false, failStart=false;
inline bool(*writer)(const uint8_t*,size_t)=nullptr;
inline int(*reader)(uint8_t*,size_t)=nullptr;
inline bool capture(bool(*sink)(const uint8_t*,size_t)) { writer=sink; error=false; return running=!failStart; }
inline bool play(int(*source)(uint8_t*,size_t),uint32_t r,uint8_t c,uint8_t) { if(r!=16000||c!=1) return false; reader=source; error=false; return running=!failStart; }
inline void shutdown() { running=false; }
inline bool busy() { return running; }
inline bool failed() { return error; }
}
''')
 (d/'HalAudioOutput.h').write_text('#pragma once\n#include <PlusAudio.h>\nnamespace HalAudioOutput { inline void shutdown() { plus_audio::shutdown(); } }\n')
 (d/'HalStorage.h').write_text(r'''#pragma once
#include <map>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
constexpr int O_RDONLY=0,O_WRONLY=1,O_CREAT=2,O_EXCL=4;
inline std::map<std::string,std::vector<uint8_t>> data;
inline bool diskReady=true, failOpen=false, shortWrite=false, failRename=false, failClose=false, failTruncate=false, failSeek=false;
inline uint64_t freeSpace=100000000;
class HalFile {
 std::string key; size_t pos=0,index=0; bool dir=false,valid=false;
 public:
 HalFile()=default;
 HalFile(std::string k,bool directory=false):key(k),dir(directory),valid(true){}
 explicit operator bool() const { return valid; }
 bool isDirectory() const { return dir; }
 bool close() { valid=false; return !failClose; }
 void flush() {}
 uint64_t fileSize64() { return data[key].size(); }
 size_t getName(char* dst,size_t n) { auto name=key.substr(key.rfind('/')+1); snprintf(dst,n,"%s",name.c_str()); return name.size(); }
 int read(void* dst,size_t n) { auto& v=data[key]; n=std::min(n,v.size()-std::min(pos,v.size())); memcpy(dst,v.data()+pos,n); pos+=n; return n; }
 size_t write(const uint8_t* src,size_t n) { if(shortWrite && n) --n; auto& v=data[key]; v.resize(std::max(v.size(),pos+n)); memcpy(v.data()+pos,src,n); pos+=n; return n; }
 bool seekSet(size_t n) { if(failSeek) return false; pos=n; return true; }
 bool truncate(uint64_t n) { if(failTruncate) return false; data[key].resize(n); return true; }
 HalFile openNextFile() { size_t i=0; for(auto& [k,v]:data) if(k.rfind("/Recordings/",0)==0 && i++==index) { ++index; return HalFile(k); } return {}; }
};
struct FakeStorage {
 bool ready() { return diskReady; }
 bool ensureDirectoryExists(const char*) { return diskReady; }
 bool getSpace(uint64_t& t,uint64_t& f) { t=freeSpace;f=freeSpace;return true; }
 bool exists(const char* p) { return data.count(p); }
 bool remove(const char* p) { return data.erase(p); }
 bool rename(const char* a,const char* b) { if(failRename||data.count(b)||!data.count(a)) return false; data[b]=std::move(data[a]);data.erase(a);return true; }
 HalFile open(const char* p,int mode=O_RDONLY) { if(failOpen) return {}; if(std::string(p)=="/Recordings") return HalFile(p,true); if(mode&O_CREAT) { if(data.count(p)&&(mode&O_EXCL)) return {};data[p]={}; } if(!data.count(p)) return {}; return HalFile(p); }
};
inline FakeStorage storage;
#define Storage storage
''')
 (d/'test.cpp').write_text(r'''#include <HalRecorder.h>
#include <RecorderWav.h>
#include <HalStorage.h>
#include <PlusAudio.h>
#include <cassert>
#include <cstdio>
using namespace HalRecorder;
void feed() { const uint8_t pcm[]={1,0,255,255,0,128,255,127}; assert(plus_audio::writer(pcm,sizeof(pcm))); }
int main() {
 uint8_t h[44]; recorder_wav::header(h,32000); assert(recorder_wav::valid(h,32044));
 assert(!recorder_wav::valid(h,32043)); h[22]=2; assert(!recorder_wav::valid(h,32044));
 recorder_wav::header(h,3); assert(!recorder_wav::valid(h,47));
 recorder_wav::header(h,0); assert(!recorder_wav::valid(h,44));
 recorder_wav::header(h,recorder_wav::maxBytes+2); assert(!recorder_wav::valid(h,recorder_wav::maxBytes+46));
 diskReady=false; assert(!startRecording() && state()==State::Error); diskReady=true;
 freeSpace=1; assert(!startRecording());freeSpace=100000000;
 failOpen=true;assert(!startRecording());failOpen=false;
 plus_audio::failStart=true;assert(!startRecording());assert(data.empty());plus_audio::failStart=false;
 assert(startRecording() && preparing() && seconds()==0); poll(); assert(preparing()); stop(); assert(state()==State::Idle && data.empty());
 assert(startRecording() && active() && preparing()); feed(); assert(!preparing() && seconds()==0); stop(); assert(state()==State::Saved && !active());
 Entry e[2];bool more;assert(list(0,e,2,more)==1 && !more);assert(e[0].seconds==0);
 auto saved=data.begin()->second;assert(saved.size()==52 && recorder_wav::valid(saved.data(),52));
 assert(play(e[0].name,35) && state()==State::Playing);
 assert(list(0,e,2,more)==0);uint8_t pcm[16];assert(plus_audio::reader(pcm,sizeof(pcm))==8);assert(memcmp(pcm,saved.data()+44,8)==0);assert(plus_audio::reader(pcm,sizeof(pcm))==0);
 plus_audio::running=false;poll();assert(state()==State::Idle);
 assert(!play("../REC_bad.wav",35) && state()==State::Error);
 for(int i=0;i<7;++i) { assert(startRecording());feed();stop(); }
 assert(list(0,e,2,more)==2 && more);assert(list(6,e,2,more)==2 && !more);
 assert(list(8,e,2,more)==0 && !more);
 assert(startRecording()); plus_audio::error=true;plus_audio::running=false;poll();assert(state()==State::Error); // no-data failure
 for(auto& [k,v]:data) assert(k.find(".part")==std::string::npos);
 assert(startRecording());feed();plus_audio::error=true;plus_audio::running=false;poll();assert(state()==State::Error); // partial saved on failure
 assert(startRecording());shortWrite=true;uint8_t b[]={1,2,3,4};assert(!plus_audio::writer(b,4));shortWrite=false;plus_audio::running=false;poll();assert(state()==State::Error);
 auto n=data.size(); assert(startRecording());feed();failRename=true;stop();failRename=false;assert(state()==State::Error && data.size()==n+1);
 assert(startRecording());feed();failSeek=true;stop();failSeek=false;assert(state()==State::Error);
 assert(startRecording());feed();failTruncate=true;stop();failTruncate=false;assert(state()==State::Error);
 assert(startRecording());feed();failClose=true;stop();failClose=false;assert(state()==State::Error);
 assert(startRecording());feed();plus_audio::shutdown();poll();assert(state()==State::Saved); // standby interruption
 for(auto& [k,v]:data) if(k.ends_with(".wav")) assert(recorder_wav::valid(v.data(),v.size()));
}
''')
 subprocess.run(['c++','-std=c++20','-DFREEINK_DEVICE_METALIO_PLUS=1','-fsanitize=address,undefined','-g',f'-I{d}',f'-I{hal}',str(d/'test.cpp'),str(hal/'HalRecorder.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
print('Recorder: WAV integrity, capture/save/replay, exclusive storage, pagination, collision avoidance, partial save, no-data, SD/audio failures and standby stop passed')
