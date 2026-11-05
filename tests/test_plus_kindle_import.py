"""Run the real HAL-based importer on desktop, then validate every ZIP/XML entry.

No attached device, serial connection, or physical card is used. Copyrighted
samples are optional local inputs; no sample content is added to the repository.
"""
import argparse
from html.parser import HTMLParser
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile
from test_plus_kindle_document import fixture

PORT = Path(__file__).resolve().parents[1]
UPSTREAM = PORT / 'firmware/rickyos-plus-1.2/upstream'
SOURCE = PORT / 'firmware/rickyos-plus-1.2/components/rickyos_application/port'
HAL = r'''#pragma once
#include <fstream>
#include <filesystem>
#include <string>
#include <cstdint>
#include <cstring>
inline int writeCountdown=-1;
class HalFile {
 std::fstream f;
 std::filesystem::directory_iterator entries,finish;
 std::string path;
 bool directory=false;
 public:
 bool open(const std::string&p,bool writing){path=p;directory=std::filesystem::is_directory(p);if(directory){entries=std::filesystem::directory_iterator(p);return true;}f.open(p,std::ios::binary|std::ios::in|(writing?std::ios::out|std::ios::trunc:std::ios::openmode(0)));return bool(f);}
 bool seek(size_t p){f.clear();f.seekg(p);return bool(f);}
 int read(void*p,size_t n){f.read(static_cast<char*>(p),n);return int(f.gcount());}
 size_t write(const uint8_t*p,size_t n){if(writeCountdown==0)return 0;if(writeCountdown>0)--writeCountdown;f.write(reinterpret_cast<const char*>(p),n);return f?n:0;}
 size_t fileSize(){auto pos=f.tellg();f.clear();f.seekg(0,std::ios::end);auto size=f.tellg();f.seekg(pos);return size;}
 uint64_t fileSize64(){return fileSize();}
 void flush(){f.flush();}
 void close(){f.close();}
 operator bool()const{return directory||f.is_open();}
 bool isDirectory()const{return directory;}
 size_t getName(char*out,size_t n){auto name=std::filesystem::path(path).filename().string();if(name.size()>=n)return n;memcpy(out,name.c_str(),name.size()+1);return name.size();}
 HalFile openNextFile(){HalFile next;if(entries!=finish){auto p=entries->path();++entries;next.open(p,false);}return next;}
};
class HalStorage {
 public:
 bool openFileForRead(const char*,const std::string&p,HalFile&f){return f.open(p,false);}
 bool openFileForWrite(const char*,const std::string&p,HalFile&f){return f.open(p,true);}
 HalFile open(const char*p){HalFile f;f.open(p,false);return f;}
 bool exists(const char*p){return std::filesystem::exists(p);}
 bool remove(const char*p){return std::filesystem::remove(p);}
 bool removeDir(const char*p){return std::filesystem::remove_all(p)>0;}
 bool ensureDirectoryExists(const char*p){std::filesystem::create_directories(p);return true;}
 bool replaceFile(const char*a,const char*b){std::filesystem::rename(a,b);return true;}
 static HalStorage&getInstance(){static HalStorage s;return s;}
};
#define Storage HalStorage::getInstance()
'''
MAIN = r'''
#include "KindleImport.h"
#include "HalStorage.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iterator>
int main(int argc,char**argv){
 assert(argc==3);std::string title,author,archive;kindle::Error error;bool rebuilt;
 assert(kindle::supported(argv[1]));assert(!kindle::supported("a.azw"));
 assert(kindle::metadata(argv[1],title,author,error));
 bool ok=kindle::prepare(argv[1],argv[2],archive,rebuilt,error);
 if(!ok)fprintf(stderr,"Importer error=%u\n",unsigned(error));assert(ok&&rebuilt);
 auto stamp=std::filesystem::last_write_time(archive);
 assert(kindle::prepare(argv[1],argv[2],archive,rebuilt,error)&&!rebuilt);
 assert(std::filesystem::last_write_time(archive)==stamp);
 {
  // A v2 marker (whole-file CRC) from an older build is verified once and adopted.
  std::ifstream in(argv[1],std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(in)),{});
  uint32_t crc=~0u;for(unsigned char c:bytes){crc^=c;for(int b=0;b<8;++b)crc=(crc>>1)^((0u-(crc&1))&0xedb88320);}crc=~crc;
  const auto marker=[&](uint32_t sum){uint8_t m[16]={'R','K','I',2};uint32_t n=bytes.size();
   for(int i=0;i<4;++i){m[4+i]=uint8_t(n>>(8*i));m[8+i]=uint8_t(sum>>(8*i));}
   std::ofstream o(std::string(argv[2])+"/kindle.v2",std::ios::binary|std::ios::trunc);o.write(reinterpret_cast<char*>(m),16);};
  marker(crc);
  assert(kindle::prepare(argv[1],argv[2],archive,rebuilt,error)&&!rebuilt);
  assert(std::filesystem::last_write_time(archive)==stamp);
  {std::ifstream m(std::string(argv[2])+"/kindle.v2",std::ios::binary);char v[4];m.read(v,4);assert(v[3]==3);}
  assert(kindle::prepare(argv[1],argv[2],archive,rebuilt,error)&&!rebuilt);
  marker(crc^1);  // a v2 marker that no longer matches the source rebuilds
  assert(kindle::prepare(argv[1],argv[2],archive,rebuilt,error)&&rebuilt);
  stamp=std::filesystem::last_write_time(archive);
 }
 // Only disposable source copies are passed to this executable.
 {std::ofstream p(std::string(argv[2])+"/progress.bin");p<<"keep-progress";}
 {std::ofstream p(std::string(argv[2])+"/cover.override");p<<"keep-user-cover";}
 {std::ofstream p(std::string(argv[2])+"/img2_0_0.jpg");p<<"stale-derived";}
 {std::ofstream p(std::string(argv[2])+"/book.bin");p<<"stale-derived";}
 {std::fstream f(argv[1],std::ios::binary|std::ios::in|std::ios::out);char c;f.get(c);f.seekp(0);f.put(c^1);}
 writeCountdown=0;
 assert(!kindle::prepare(argv[1],argv[2],archive,rebuilt,error));
 assert(std::filesystem::last_write_time(std::string(argv[2])+"/kindle.epub")==stamp);
 writeCountdown=-1;
 assert(kindle::prepare(argv[1],argv[2],archive,rebuilt,error)&&rebuilt);
 assert(!std::filesystem::exists(std::string(argv[2])+"/img2_0_0.jpg"));
 assert(!std::filesystem::exists(std::string(argv[2])+"/book.bin"));
 assert(std::filesystem::exists(std::string(argv[2])+"/cover.override"));
 {std::ifstream p(std::string(argv[2])+"/progress.bin");std::string s;std::getline(p,s);assert(s=="keep-progress");}
 bool stop=true;
 assert(!kindle::prepare(argv[1],argv[2],archive,rebuilt,error,{&stop,[](void*p){return *static_cast<bool*>(p);}}));
 assert(error==kindle::Error::Cancelled);
 puts("Production importer: cold open, warm cache reuse and cancellation passed");
}
'''

class Text(HTMLParser):
    def __init__(self):super().__init__(convert_charrefs=True);self.parts=[];self.skip=0
    def handle_starttag(self,tag,attrs):
        if tag in ('head','script','style','guide'):self.skip+=1
    def handle_endtag(self,tag):
        if tag in ('head','script','style','guide') and self.skip:self.skip-=1
    def handle_data(self,data):
        if not self.skip:self.parts.append(data)
    def normalized(self):return re.sub(r'\s+','',''.join(self.parts))

REFERENCE = r'''
const fs=require('fs'),vm=require('vm');
const script=fs.readFileSync(process.argv[1],'utf8').replaceAll('export ','');
const ref=vm.runInNewContext(script+';({getStruct,MOBI_HEADER,huffcdic,getVarLenFromEnd,countBitsSet,decompressPalmDOC,getIndexData})',{TextDecoder,TextEncoder,Blob,console});
(async()=>{
const b=fs.readFileSync(process.argv[2]);const count=b.readUInt16BE(76);
const offsets=Array.from({length:count},(_,i)=>b.readUInt32BE(78+i*8));
async function load(i){const p=b.subarray(offsets[i],offsets[i+1]??b.length);return p.buffer.slice(p.byteOffset,p.byteOffset+p.length);}
const head=await load(0),mobi=ref.getStruct(ref.MOBI_HEADER,head),h=Buffer.from(head);
const compression=h.readUInt16BE(0),version=h.readUInt32BE(36);
const decompress=compression===17480?await ref.huffcdic(mobi,load):compression===2?ref.decompressPalmDOC:x=>x;
const chunks=[];
for(let i=1;i<=h.readUInt16BE(8);i++){
let a=new Uint8Array(await load(i));
for(let j=0;j<ref.countBitsSet(mobi.trailingFlags>>>1);++j)a=a.subarray(0,-ref.getVarLenFromEnd(a));
if(mobi.trailingFlags&1)a=a.subarray(0,-((a[a.length-1]&3)+1));chunks.push(Buffer.from(decompress(a)));
}
const raw=Buffer.concat(chunks);let sections;
if(version===8){
 const skels=(await ref.getIndexData(h.readUInt32BE(252),load)).table;
 const frags=(await ref.getIndexData(h.readUInt32BE(248),load)).table;let cursor=0;
 sections=skels.map(({tagMap:t})=>{
  const off=t[6][0],len=t[6][1],rows=frags.slice(cursor,cursor+=t[1][0]);
  const total=len+rows.reduce((n,r)=>n+r.tagMap[6][1],0),data=raw.subarray(off,off+total);
  let out=data.subarray(0,len);
  for(const r of rows){const insert=Number(r.name)-off,begin=len+r.tagMap[6][0];
   out=Buffer.concat([out.subarray(0,insert),data.subarray(begin,begin+r.tagMap[6][1]),out.subarray(insert)]);}
  return out.toString('utf8');
 });
}else sections=[raw.subarray(0,h.readUInt32BE(4)).toString('utf8')];
process.stdout.write(JSON.stringify(sections));
})();
'''

def validate(path,expected=None,original=None):
    with zipfile.ZipFile(path) as book:
        assert book.testzip() is None
        assert book.read('mimetype') == b'application/epub+zip'
        names = set(book.namelist())
        xml = {n: ET.fromstring(book.read(n)) for n in names if n.endswith(('.xml','.xhtml','.opf','.ncx'))}
        ns = {'o':'http://www.idpf.org/2007/opf','n':'http://www.daisy.org/z3986/2005/ncx/'}
        opf = xml['content.opf']
        manifest = {e.attrib['id']:e.attrib['href'] for e in opf.findall('o:manifest/o:item',ns)}
        for name in manifest.values():
            assert name in names, name
        spine = [manifest[e.attrib['idref']] for e in opf.findall('o:spine/o:itemref',ns)]
        assert spine
        for entry in xml['toc.ncx'].findall('.//n:content',ns):
            name,anchor = entry.attrib['src'].split('#')
            assert any(e.attrib.get('id')==anchor for e in xml[name].iter()), (name,anchor)
        images=0
        for name in spine:
            content=book.read(name).decode()
            assert 'kindle:' not in content
            for e in xml[name].iter():
                for key in ('src','href'):
                    if key not in e.attrib: continue
                    target=e.attrib[key].split('#')[0]
                    if target and ':' not in target:
                        assert target in names, (name,target)
                        if '#' in e.attrib[key] and target.endswith('.xhtml'):
                            anchor=e.attrib[key].split('#',1)[1]
                            assert any(node.attrib.get('id')==anchor for node in xml[target].iter()), (name,target,anchor)
                if e.tag.endswith('}img'): images+=1
        if expected is not None:
            before=Text();after=Text()
            for section in expected:before.feed(section)
            for name in spine:after.feed(book.read(name).decode())
            assert before.normalized()==after.normalized(), 'Chapter text differs from the independent reference decoder/reconstructor'
            print('Independent reference comparison: complete book text preserved')
        if original:
            data=original.read_bytes();count=struct.unpack_from('>H',data,76)[0]
            offsets=[struct.unpack_from('>I',data,78+i*8)[0] for i in range(count)]+[len(data)]
            start=struct.unpack_from('>I',data,offsets[0]+108)[0]
            for name in names:
                match=re.fullmatch(r'r(\d+)\.(?:jpg|png)',name)
                if match:
                    index=start+int(match[1]);assert book.read(name)==data[offsets[index]:offsets[index+1]],name
            print('Source images: resource bytes preserved without recompression')
        print(f'Validated archive: {len(spine)} chapters, {len(xml["toc.ncx"].findall(".//n:navPoint",ns))} TOC entries, {images} image references')

def run(samples,reference):
    with tempfile.TemporaryDirectory(prefix='plus-kindle-import-') as directory:
        work=Path(directory)
        (work/'HalStorage.h').write_text(HAL)
        (work/'Logging.h').write_text('#pragma once\n#include <cstdio>\n#define LOG_ERR(tag,fmt,...) fprintf(stderr,fmt "\\n",##__VA_ARGS__)\n#define LOG_INF(...) ((void)0)\n')
        (work/'main.cpp').write_text(MAIN)
        exe=work/'test'
        subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-O1','-g',
                        '-DSIMULATOR=1','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                        '-I'+str(work),'-I'+str(SOURCE),'-I'+str(UPSTREAM/'lib/Memory'),
                        '-I'+str(UPSTREAM/'lib/Epub'),
                        str(work/'main.cpp'),str(SOURCE/'KindleDocument.cpp'),str(SOURCE/'KindleImport.cpp'),
                        str(UPSTREAM/'lib/Epub/Epub/htmlEntities.cpp'),'-o',str(exe)],check=True)
        synthetic=work/'synthetic.mobi'
        synthetic.write_bytes(fixture('<html><body><h1>测试</h1><p>中文 &amp; ASCII</p></body></html>'.encode()))
        subprocess.run([str(exe),str(synthetic),str(work/'synthetic-cache')],check=True)
        validate(work/'synthetic-cache/kindle.epub')
        # Fixed-layout books (Kmoe comics: one image per page) import like any other book.
        fixed=work/'fixed-layout.mobi'
        fixed.write_bytes(fixture(b'<html><body><p>page</p></body></html>',
                                  exth=((122,b'true'),(123,b'comic'),(527,b'rtl'))))
        subprocess.run([str(exe),str(fixed),str(work/'fixed-cache')],check=True)
        validate(work/'fixed-cache/kindle.epub')
        for name,html,encoding in (
            ('entities','<html><body><p>&copy; &mdash; &nbsp; 中文</p></body></html>'.encode(),65001),
            ('legacy-html',b'<html><body><mbp:pagebreak/><p align=center>one<p>two<br>three</body></html>',65001),
            ('latin',b'<html><body><p>caf\xe9 \x93yes\x94 &copy;</p></body></html>',1252),
        ):
            data=bytearray(fixture(html));first=struct.unpack_from('>I',data,78)[0]
            struct.pack_into('>I',data,first+28,encoding)
            sample=work/f'{name}.mobi';sample.write_bytes(data)
            cache=work/f'{name}-cache'
            subprocess.run([str(exe),str(sample),str(cache)],check=True)
            validate(cache/'kindle.epub')
        for i,sample in enumerate(samples):
            cache=work/f'cache{i}'
            copy=work/f'sample{i}{sample.suffix}';shutil.copy2(sample,copy)
            subprocess.run([str(exe),str(copy),str(cache)],check=True)
            expected=json.loads(subprocess.check_output(['node','-e',REFERENCE,str(reference),str(sample)],text=True)) if reference else None
            validate(cache/'kindle.epub',expected,sample)
    print('Kindle import: ASan/UBSan + complete archive/TOC/image reference validation passed')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--samples',nargs='*',type=Path,default=[])
    parser.add_argument('--reference',type=Path,help='Optional locally downloaded pinned foliate mobi.js for differential tests')
    args=parser.parse_args();run(args.samples,args.reference)
