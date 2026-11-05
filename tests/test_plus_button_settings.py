"""Exercise actual Plus JSON save/load fragments with the real ArduinoJson."""
from pathlib import Path
import subprocess, tempfile
u=Path('firmware/rickyos-plus-1.2/upstream')
s=(u/'src/CrossPointSettings.cpp').read_text()
start=s.index('#if FREEINK_DEVICE_METALIO_PLUS',s.index('void CrossPointSettings::toJson'))
save=s[start:s.index('#endif',start)+6]
start=s.index('#if FREEINK_DEVICE_METALIO_PLUS',s.index('bool CrossPointSettings::fromJson'))
load=s[start:s.index('#endif',start)+6]
program='''#include <ArduinoJson.h>
#include <cassert>
#include "PlusButtonInput.h"
#define FREEINK_DEVICE_METALIO_PLUS 1
struct Settings {
 uint8_t plusTouchFeedback=1;
 static uint8_t clamp(uint8_t v,uint8_t max,uint8_t def){return v<max?v:def;}
 uint8_t plusKeyActions[10]={0,1,0,1,0,1,0,1,0,1};
 void save(JsonVariant doc){
'''+save+'''
}
 void load(JsonVariantConst doc){
'''+load+'''
}
};
int main(){
 JsonDocument doc;
 Settings a,b;
 for(int i=0;i<10;i++)a.plusKeyActions[i]=i%plus_keys::Count;
 a.plusTouchFeedback=0;
 a.save(doc.to<JsonObject>());b.load(doc.as<JsonVariantConst>());
 assert(b.plusTouchFeedback==0);
 for(int i=0;i<10;i++)assert(a.plusKeyActions[i]==b.plusKeyActions[i]);
 deserializeJson(doc,"{}");b.load(doc.as<JsonVariantConst>());
 assert(b.plusTouchFeedback==1);
 for(int i=0;i<10;i++)assert(b.plusKeyActions[i]==(i%2?1:0));
 deserializeJson(doc,R"({"plusKeyActions":[255,255,-1,"Home",null,true,3]})");b.load(doc.as<JsonVariantConst>());
 for(int i=0;i<10;i++)assert(b.plusKeyActions[i]==(i==6?3:(i%2?1:0)));
 deserializeJson(doc,R"({"plusKeyActions":"invalid"})");b.load(doc.as<JsonVariantConst>());
 for(int i=0;i<10;i++)assert(b.plusKeyActions[i]==(i%2?1:0));
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(program)
 subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined','-I'+str(u/'src/util'),'-I.local/dependencies/ArduinoJson/src',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual Plus JSON/button/vibration persistence passed: round trip, missing/short/invalid array, invalid action/type, defaults.')
