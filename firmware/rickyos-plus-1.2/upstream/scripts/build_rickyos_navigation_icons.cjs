#!/usr/bin/env node
/** Maintained Lucide SVGs (ISC license) -> native 1-bpp resources, not runtime SVGs.
 * Developer-only dependency: sharp 0.35.4 (librsvg 2.62.91).
 * Run: NODE_PATH=/path/to/node_modules node scripts/build_rickyos_navigation_icons.cjs
 * Optional: --out header.h --preview preview.png
 * The checked-in header makes device/simulator builds independent of Node/sharp.
 */
const fs = require('node:fs');
const path = require('node:path');
const sharp = require('sharp');
const root = path.resolve(__dirname, '..');
const sourceDir = path.join(root, 'src/components/icons/sources/rickyos');
const names = ['home', 'library', 'apps', 'settings', 'statistics', 'storage'];
const labels = ['首页', '书库', '应用', '设置', '统计'];
const args = process.argv.slice(2);
let output = path.join(root, 'src/components/icons/rickyNavigationIcons.h');
let preview;
for (let i = 0; i < args.length; ++i) {
  if (args[i] === '--out' && args[i + 1]) output = path.resolve(args[++i]);
  else if (args[i] === '--preview' && args[i + 1]) preview = path.resolve(args[++i]);
  else throw new Error('Usage: build_rickyos_navigation_icons.cjs [--out header.h] [--preview preview.png]');
}

async function main() {
  const lines = ['#pragma once', '', '#include <cstdint>', '',
    '// Generated from maintained Lucide SVGs (ISC license) by scripts/build_rickyos_navigation_icons.cjs.',
    '// Edit sources/rickyos/*.svg and regenerate; never edit these bytes manually.',
    '// MSB-first, 1 = transparent/white, 0 = ink. Upright; no runtime resampling.', ''];
  const gallery = [];
  for (const [index, name] of names.entries()) {
    const svg = fs.readFileSync(path.join(sourceDir, name + '.svg'));
    for (const size of [40, 56]) {
      const nativeSvg = Buffer.from(svg.toString().replace(/<svg\b/, `<svg width="${size}" height="${size}"`));
      const { data, info } = await sharp(nativeSvg).flatten({ background: '#fff' })
        .greyscale().raw().toBuffer({ resolveWithObject: true });
      if (info.channels !== 1 || info.width !== size || info.height !== size) {
        throw new Error('Expected native-size grayscale raster');
      }
      const rowBytes = Math.ceil(size / 8);
      const bits = Buffer.alloc(rowBytes * size, 255);
      for (let y = 0; y < size; ++y) for (let x = 0; x < size; ++x) {
        if (data[y * size + x] < 110) bits[y * rowBytes + (x >> 3)] &= ~(0x80 >> (x & 7));
      }
      lines.push(`// ${name}; ${size} x ${size}; ${bits.length} bytes in Flash.`);
      lines.push(`static constexpr uint8_t ricky_nav_${name}_${size}[] = {`);
      for (let offset = 0; offset < bits.length; offset += 16) {
        lines.push('    ' + [...bits.subarray(offset, offset + 16)]
          .map(byte => '0x' + byte.toString(16).padStart(2, '0').toUpperCase()).join(', ') + ',');
      }
      lines.push('};', '');
      if (size === 56) {
        // Preview the actual thresholded resource, enlarged with nearest-neighbor only.
        const pixels = Buffer.alloc(data.length);
        for (let p = 0; p < pixels.length; ++p) pixels[p] = data[p] < 110 ? 0 : 255;
        const png = await sharp(pixels, { raw: { width: size, height: size, channels: 1 } })
          .resize(112, 112, { kernel: 'nearest' }).png().toBuffer();
        gallery.push({ input: png, left: 69 + index * 244, top: 114 });
      }
    }
  }
  fs.writeFileSync(output, lines.join('\n'));
  if (preview) {
    const title = `<svg xmlns="http://www.w3.org/2000/svg" width="1220" height="320">
      <rect width="1220" height="320" fill="white"/>
      <text x="48" y="52" font-family="sans-serif" font-size="26" fill="black">RickyOS · Navigation</text>
      <text x="48" y="82" font-family="sans-serif" font-size="16" fill="#555">56 px · Native monochrome pixels shown at 2×</text>
      ${labels.map((label, i) => `<text x="${125 + i * 244}" y="274" text-anchor="middle" font-family="PingFang SC,sans-serif" font-size="24" fill="black">${label}</text>`).join('')}
    </svg>`;
    await sharp(Buffer.from(title)).composite(gallery).png().toFile(preview);
  }
  console.log(`Generated ${output}: ${names.length} icons x 2 native sizes.`);
}
main().catch(error => { console.error(error); process.exitCode = 1; });
