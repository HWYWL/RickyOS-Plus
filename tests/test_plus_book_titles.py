"""Execute real book-title selection with actual font repertoire; check UI roles."""
from pathlib import Path
import re
import subprocess
import tempfile

PORT=Path(__file__).resolve().parents[1]
APP=PORT/'firmware/rickyos-plus-1.2/upstream'


def method(source, signature):
    begin=source.index(signature);brace=source.index('{',begin);end=brace+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[begin:end]


design=(APP/'src/components/RickyDesignUi.h').read_text()
main=(APP/'src/main.cpp').read_text()
home=(APP/'src/components/RickyHomeUi.cpp').read_text()
library=(APP/'src/activities/library/LibraryListActivity.cpp').read_text()
assert 'renderer.insertFont(RickyDesignUi::BOOK_TITLE, bookTitleFamily)' in main
assert 'bookTitleFont(&ricky_book_12_medium)' in main
assert home.count('design::bookTitle(renderer,')==2
assert library.count('RickyDesignUi::bookTitle(renderer,')==2
assert 'RickyDesignUi::BOOK_TITLE)}' in library
assert 'RickyDesignUi::secondaryText(renderer, detailRect' in library
assert 'design::secondaryText(renderer' in home
assert 'SyntheticBold' not in method(design,'inline void bookTitle(')
assert 'ricky_book_12_medium' in (PORT/'scripts/check_plus_font_resources.py').read_text()
code=r'''
#include <cassert>
#include <cstdint>
#include <map>
#include <Utf8.h>
#include "EpdFontData.h"
namespace faces {
#include "ricky_book_12_medium.h"
#include "ricky_headline_14_medium.h"
}
struct Face {
 const EpdFontData* data;
 bool hasCodepoint(uint32_t cp)const{
  for(size_t i=0;i<data->intervalCount;++i)if(data->intervals[i].first<=cp&&cp<=data->intervals[i].last)return true;
  return false;
 }
};
struct GfxRenderer {
 std::map<int,Face> fonts;
 const auto& getFontMap()const{return fonts;}
};
namespace RickyDesignUi {
constexpr int BODY=0x524401,HEADLINE=0x524403,BOOK_TITLE=0x524406;
''' + method(design,'inline int fontFor(')+'\n'+method(design,'inline int bookTitleFontFor(')+r'''
}
int main(){
 using namespace RickyDesignUi;
 GfxRenderer r;
 r.fonts.emplace(BOOK_TITLE,Face{&faces::ricky_book_12_medium});
 r.fonts.emplace(HEADLINE,Face{&faces::ricky_headline_14_medium});
 for(const char* title:{"银河英雄传说","超神机械师","阅读与生活","随身书房","RickyOS 使用指南","他們"}){
  assert(bookTitleFontFor(r,title)==BOOK_TITLE);
 }
 for(const char* title:{"银河英雄传说","超神机械师","阅读与生活","随身书房"})
  assert(bookTitleFontFor(r,title,HEADLINE)==BOOK_TITLE);
 assert(bookTitleFontFor(r,"间客",HEADLINE)==HEADLINE);
 assert(bookTitleFontFor(r,"间客")==BOOK_TITLE);
 // Out-of-repertoire text retains the existing whole-run fallback policy.
 assert(bookTitleFontFor(r,"书名🙂")==BODY);
 r.fonts.erase(BOOK_TITLE);
 assert(bookTitleFontFor(r,"银河英雄传说")==BODY);
 assert(bookTitleFontFor(r,"间客",HEADLINE)==HEADLINE);
}
'''
with tempfile.TemporaryDirectory(prefix='plus-book-title-') as temp:
    p=Path(temp);(p/'test.cpp').write_text(code)
    subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-I'+str(APP/'lib/EpdFont'),'-I'+str(APP/'lib/EpdFont/builtinFonts'),
                    '-I'+str(APP/'lib/Utf8'),str(p/'test.cpp'),str(APP/'lib/Utf8/Utf8.cpp'),
                    '-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('Actual book-title selection: Medium for full Chinese titles, retained large headlines, whole-run unsupported fallback; home/library roles separated; ASan/UBSan passed')
