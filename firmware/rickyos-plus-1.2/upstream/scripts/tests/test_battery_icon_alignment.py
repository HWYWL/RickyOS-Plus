"""Center the actual status-numeral digit ink (Read Pico and Plus share it), not the font line box."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
app = ROOT


def rows(text, suffix):
    body = re.search(r'\w+' + suffix + r'\[\d*\]\s*=\s*\{(.*?)\n\};', text, re.S)[1]
    return [tuple(int(v.strip(), 0) for v in row.split(','))
            for row in re.findall(r'\{\s*([^{}]+?)\s*\}', body)]


faces = []
for name in ('ricky_caption_10_regular', 'notosans_12_regular'):
    text = (app / 'lib/EpdFont/builtinFonts' / (name + '.h')).read_text()
    metadata = text.split('static const EpdFontData ' + name + ' = {')[1].split('};')[0].split(',')
    advance, ascent = int(metadata[4]), int(metadata[5])
    glyphs, intervals = rows(text, 'Glyphs'), rows(text, 'Intervals')
    lookup = {cp: glyphs[offset + cp - first] for first, last, offset in intervals for cp in range(first, last + 1)}
    selected = {cp: lookup[cp] for cp in map(ord, '0123456789%')}
    top = min(ascent - g[4] for g in selected.values())
    bottom = max(ascent - g[4] + g[1] for g in selected.values())
    faces.append((name, advance, ascent, selected, top, bottom))

with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    code = '''#include <map>
#include <cassert>
#include <cstdlib>
#include "components/BatteryStatusLayout.h"
struct Glyph {int width,height,top;}; struct Data {int ascender;};
struct Face {Data data;std::map<int,Glyph> glyphs;
 const Data* getData() const {return &data;}
 const Glyph* getGlyph(int cp)const {auto i=glyphs.find(cp);return i==glyphs.end()?nullptr:&i->second;}};
struct Renderer {std::map<int,Face> faces;int line=0;
 const auto& getFontMap() const {return faces;} int getLineHeight(int)const{return line;}};
int main(){
'''
    for index, (name, advance, ascent, glyphs, top, bottom) in enumerate(faces):
        entries = ','.join('{%d,{%d,%d,%d}}' % (cp, g[0], g[1], g[4]) for cp, g in glyphs.items())
        code += '{Renderer r;r.line=%d;r.faces.emplace(%d,Face{Data{%d},{%s}});\n' % (advance, index, ascent, entries)
        code += '''for(int y:{0,7,32,100})for(int h:{12,20,24}) {
 int icon=ricky::battery::iconTop(r,ID,y,h);
 assert(std::abs((icon-y)*2+h-(TOP+BOTTOM))<=1);
}
int first=ricky::battery::iconTop(r,ID,0,20);
r.line+=40;assert(ricky::battery::iconTop(r,ID,0,20)==first); // line leading is not ink
r.faces.clear();assert(ricky::battery::iconTop(r,ID,3,20)==3+(r.line-20)/2);
}
'''.replace('ID', str(index)).replace('TOP', str(top)).replace('BOTTOM', str(bottom))
        print(name, 'ink bounds', (top, bottom), 'old icon top 8 ->', (top + bottom - 20) // 2)
    code += '}\n'
    (temp / 'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + str(app / 'src'), str(temp / 'test.cpp'), '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], check=True)
print('Actual status numeric fonts: icon/ink centers within half a pixel, stable leading and fallback passed')
