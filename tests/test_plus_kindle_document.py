"""Compile the portable production parser under ASan/UBSan.

Fixtures contain only synthetic text. Optional user samples stay local and
are never packaged with the repository: --samples PATH.mobi PATH.azw3.
"""
from pathlib import Path
import argparse
import struct
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
SOURCE = PORT / 'firmware/rickyos-plus-1.2/components/rickyos_application/port'
MEMORY = PORT / 'firmware/rickyos-plus-1.2/upstream/lib/Memory'
STUB = r'''#pragma once
#include <memory>
#include <cstdlib>
#include <new>
#include <utility>
inline int allocations=0,failAt=0,live=0;
template<class T,class...A>auto makeUniqueNoThrow(A&&...a){
 if(++allocations==failAt)return std::unique_ptr<T>{};
 return std::unique_ptr<T>(new(std::nothrow)T(std::forward<A>(a)...));}
namespace memory {
struct Free {void operator()(uint8_t*p)const{if(p){free(p);--live;}}};
using ByteBuffer=std::unique_ptr<uint8_t,Free>;
inline bool psramHasHeadroom(size_t n,size_t,size_t){return n<=4*1024*1024;}
inline ByteBuffer makePsramByteBufferUninitializedNoThrow(size_t n){
 if(++allocations==failAt)return {};auto*p=static_cast<uint8_t*>(malloc(n));if(p)++live;return ByteBuffer(p);
}
}
'''
MAIN = r'''
#include "KindleDocument.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
using namespace kindle;
struct File {std::ifstream stream;uint32_t size;bool shortRead=false;};
bool readAt(void*ctx,uint32_t at,void*out,size_t n){auto&f=*static_cast<File*>(ctx);assert(at<=f.size&&n<=f.size-at);
 if(f.shortRead)return false;f.stream.clear();f.stream.seekg(at);f.stream.read(static_cast<char*>(out),n);return size_t(f.stream.gcount())==n;}
bool writeTo(void*ctx,const uint8_t*p,size_t n){auto&v=*static_cast<std::vector<uint8_t>*>(ctx);v.insert(v.end(),p,p+n);return true;}
bool countRows(void*ctx,uint32_t i,const IndexRow&){auto&count=*static_cast<uint32_t*>(ctx);assert(i==count++);return true;}
int main(int argc,char**argv){
 assert(argc>=3);File f;f.stream.open(argv[1],std::ios::binary);assert(f.stream);
 f.stream.seekg(0,std::ios::end);f.size=static_cast<uint32_t>(f.stream.tellg());
 auto doc=std::make_unique<Document>();bool opened=doc->open({&f,f.size,readAt,{}});
 if(!strcmp(argv[2],"reject")){assert(!opened&&doc->error()!=Error::None);doc.reset();assert(live==0);return 0;}
 assert(opened);
 std::vector<uint8_t> output;bool ok=doc->decodeText({&output,writeTo});
 if(!ok)fprintf(stderr,"decode error=%d after=%zu expected=%u\n",int(doc->error()),output.size(),doc->header().textBytes);
 assert(ok);assert(output.size()==doc->header().rawBytes);
 if(strcmp(argv[2],"sample")){
   std::ifstream expected(argv[2],std::ios::binary);std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(expected),{}};
   assert(output==bytes);
 }else{
   if(argc>3){std::ofstream dump(argv[3],std::ios::binary);dump.write(reinterpret_cast<const char*>(output.data()),output.size());}
   uint32_t skels=0,frags=0,ncx=0;
   if(doc->header().version==8){assert(doc->index(doc->header().skeletons,countRows,&skels));assert(doc->index(doc->header().fragments,countRows,&frags));}
   if(doc->header().ncx!=0xffffffff)assert(doc->index(doc->header().ncx,countRows,&ncx));
   printf("sample: version=%u compression=%u decoded=%zu skeletons=%u fragments=%u ncx=%u\n",doc->header().version,doc->header().compression,output.size(),skels,frags,ncx);
 }
 doc.reset();assert(live==0);
 // Fail every allocation in the actual open/decode path. Existing buffers must
 // be released; OOM is returned rather than aborting or publishing output.
 const int checkpoints=allocations;
 for(int i=1;i<=checkpoints;++i){
   allocations=0;failAt=i;doc=std::make_unique<Document>();
   output.clear();bool success=doc->open({&f,f.size,readAt,{}})&&doc->decodeText({&output,writeTo});
   if(!success)assert(doc->error()==Error::OutOfMemory);
   doc.reset();assert(live==0);
 }
 failAt=0;
 f.shortRead=true;doc=std::make_unique<Document>();assert(!doc->open({&f,f.size,readAt,{}})&&doc->error()==Error::Io);doc.reset();assert(live==0);
 bool stop=true;doc=std::make_unique<Document>();assert(!doc->open({&f,f.size,readAt,{&stop,[](void*p){return *static_cast<bool*>(p);}}}));assert(doc->error()==Error::Cancelled);doc.reset();assert(live==0);
 const uint8_t literals[]={3,'a','b','c',0x80,0x18,0xe1};uint8_t plain[8];size_t n;
 assert(Document::palmDoc(literals,sizeof(literals),plain,sizeof(plain),n));assert(n==8&&!memcmp(plain,"abcabc a",8));
 for(size_t cap=0;cap<8;++cap)assert(!Document::palmDoc(literals,sizeof(literals),plain,cap,n));
 const uint8_t bad[]={0x80,0x08};assert(!Document::palmDoc(bad,2,plain,8,n));
}
'''

def fixture(text, compression=1, text_record=None, extras=(), exth=()):
    title = b'Synthetic reader test'
    # Optional EXTH records (type, bytes) sit right after the 232-byte MOBI header.
    block = b''.join(struct.pack('>II', kind, 8 + len(value)) + value for kind, value in exth)
    block = b'EXTH' + struct.pack('>II', 12 + len(block), len(exth)) + block if exth else b''
    record = bytearray(248 + len(block) + len(title))
    struct.pack_into('>HHIHHH', record, 0, compression, 0, len(text), 1, 4096, 0)
    record[16:20] = b'MOBI'
    for offset, value in {20:232, 24:2, 28:65001, 36:6, 84:248 + len(block), 88:len(title), 108:0xffffffff,
                          128:64 if exth else 0, 244:0xffffffff}.items():
        struct.pack_into('>I', record, offset, value)
    record[248:248 + len(block)] = block
    record[248 + len(block):] = title
    if compression == 17480:
        struct.pack_into('>II', record, 112, 2, 2)
    records = [record, text if text_record is None else text_record, *extras]
    pdb = bytearray(78)
    pdb[60:68] = b'BOOKMOBI'
    struct.pack_into('>H', pdb, 76, len(records))
    index = bytearray()
    at = 78 + len(records) * 8
    for r in records:
        index += struct.pack('>II', at, 0)
        at += len(r)
    return pdb + index + b''.join(records)

def run(samples):
    with tempfile.TemporaryDirectory(prefix='plus-kindle-') as temp:
        work = Path(temp)
        (work / 'Memory.h').write_text(STUB)
        (work / 'main.cpp').write_text(MAIN)
        exe = work / 'test'
        subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror','-O1','-g',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                        '-I'+str(work),'-I'+str(SOURCE),'-I'+str(MEMORY),str(work/'main.cpp'),
                        str(SOURCE/'KindleDocument.cpp'),'-o',str(exe)],check=True)
        cases = [('plain', '<html><body>中文 test</body></html>'.encode(), 1, None, ())]
        cases += [('palm', b'abcabc a', 2, bytes([3,97,98,99,0x80,0x18,0xe1]), ())]
        # Eight fixed three-bit codes, with no ambiguous trailing padding.
        huff = bytearray(16+1024+256)
        huff[:4] = b'HUFF';struct.pack_into('>III',huff,4,16,16,1040)
        for i in range(256):struct.pack_into('>I',huff,16+4*i,(7<<8)|128|3)
        cdic = bytearray(b'CDIC'+struct.pack('>III',16,8,3))
        cdic += b''.join(struct.pack('>H',16+i*3) for i in range(8))
        cdic += b''.join(struct.pack('>H',0x8001)+bytes([ord('a')+i]) for i in range(8))
        cases += [('huff', b'abcdefgh',17480,int(''.join(f'{7-i:03b}' for i in range(8)),2).to_bytes(3,'big'),(huff,cdic))]
        for name,text,compression,packed,extras in cases:
            (work/(name+'.mobi')).write_bytes(fixture(text,compression,packed,extras))
            (work/(name+'.txt')).write_bytes(text)
            subprocess.run([str(exe),str(work/(name+'.mobi')),str(work/(name+'.txt'))],check=True)
        for kind in ('encrypted','unsupported','bad-table'):
            data=bytearray(fixture(b'test'))
            first=struct.unpack_from('>I',data,78)[0]
            if kind=='encrypted':struct.pack_into('>H',data,first+12,1)
            elif kind=='unsupported':struct.pack_into('>H',data,first,999)
            else:struct.pack_into('>I',data,78,0xffffffff)
            bad=work/(kind+'.mobi');bad.write_bytes(data)
            subprocess.run([str(exe),str(bad),'reject'],check=True)
        for sample in samples:
            subprocess.run([str(exe),str(sample),'sample',str(PORT/'.local'/('kindle-'+sample.suffix[1:]+'.raw'))],check=True)
    print('Production Kindle core: PalmDOC/HUFF exact synthetic output, bounds, short-read, cancellation and ownership passed (ASan/UBSan)')

if __name__ == '__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--samples',nargs='*',type=Path,default=[])
    run(parser.parse_args().samples)
