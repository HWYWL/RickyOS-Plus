"""Exercise actual source changes, host privacy and repaint suppression."""
from pathlib import Path
import subprocess
import tempfile

root = Path('firmware/rickyos-plus-1.2/upstream')
source = (root / 'src/activities/settings/FontDownloadActivity.cpp').read_text()
start = source.index('void FontDownloadActivity::updateDownloadSource(')
end = source.index('\nFontDownloadActivity::DownloadResult', start)
methods = source[start:end]
assert 'downloadSourceLabel' not in source
assert 'STR_FONT_DOWNLOAD_SOURCE' not in source
assert 'sourceHeight' not in source
code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include "FontDownloadSource.h"
std::string loggedHost; int logs=0;
void logHost(const char*,const char*,const char* host){loggedHost=host;++logs;}
#define LOG_INF(...) logHost(__VA_ARGS__)
#define tr(id) #id
uint32_t millis(){return 42;}
struct RenderLock {template<class T> explicit RenderLock(T&){} };
struct FontDownloadActivity {
 char downloadSourceHost_[256]{}; uint32_t lastProgressRefreshMs_=0; int frames=0;
 void requestUpdateAndWait(){++frames;}
 void updateDownloadSource(const char*);
};
''' + methods + r'''
int main(){
 FontDownloadActivity activity;
 activity.updateDownloadSource(nullptr); assert(activity.frames==0);
 activity.updateDownloadSource("RICKYOS-1309217881.COS.AP-GUANGZHOU.MYQCLOUD.COM");
 assert(logs==1 && loggedHost=="rickyos-1309217881.cos.ap-guangzhou.myqcloud.com");
 activity.updateDownloadSource("rickyos-1309217881.cos.ap-guangzhou.myqcloud.com");
 assert(logs==1); // Repeated host does not spam the diagnostic log.
 activity.updateDownloadSource("cdn.jsdelivr.net");
 assert(loggedHost=="cdn.jsdelivr.net");
 activity.updateDownloadSource("fastly.jsdelivr.net");
 assert(loggedHost=="fastly.jsdelivr.net");
 activity.updateDownloadSource("github.com");
 assert(loggedHost=="github.com");
 activity.updateDownloadSource("release-assets.githubusercontent.com");
 assert(loggedHost=="release-assets.githubusercontent.com");
 activity.updateDownloadSource("user:private@cdn.jsdelivr.net");
 assert(std::string(activity.downloadSourceHost_)=="cdn.jsdelivr.net");
 assert(loggedHost=="cdn.jsdelivr.net");
 activity.updateDownloadSource("cdn.jsdelivr.net.evil.test");
 assert(loggedHost=="cdn.jsdelivr.net.evil.test");
 activity.updateDownloadSource("unknown.test?secret=yes");
 assert(std::string(activity.downloadSourceHost_)=="unknown.test");
 assert(loggedHost=="unknown.test");
 activity.updateDownloadSource(std::string(1000,'x').c_str());
 assert(std::strlen(activity.downloadSourceHost_)==255);
 activity.updateDownloadSource(""); assert(activity.downloadSourceHost_[0]==0);
 assert(activity.frames==0 && activity.lastProgressRefreshMs_==0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp)
    (p / 'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(root / 'src/network'),
                    str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
print('Font source: diagnostic-only sanitized hosts; no source UI, repaint or progress timer reset (ASan/UBSan)')
