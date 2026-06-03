#!/usr/bin/env node
// Pack a built web directory into a single flat archive for embedding straight
// into the firmware image (replaces the old SPIFFS 'web' partition).
//
//   node pack-web.mjs <web_image_dir> <out.bin>
//
// Text assets (html/js/css/…) are gzipped at level 9 when that's smaller;
// already-compressed assets (woff2/png/…) are stored raw. Source maps are
// never embedded. The device serves each entry verbatim, setting
// `Content-Encoding: gzip` for the gzipped ones (every browser accepts it).
//
// Archive layout (little-endian), parsed by main/web_assets.c:
//   magic   "WBND"            (4)
//   version u32 = 1           (4)
//   count   u32               (4)
//   directory[count]:
//     path_len u16, path bytes, '\0'
//     mime_len u16, mime bytes, '\0'
//     flags    u8   (bit0 = gzip)
//     data_off u32  (absolute offset from blob start)
//     data_len u32
//   data section (each entry's bytes at its data_off)

import { readdirSync, statSync, readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, relative, sep, dirname } from 'node:path';
import { gzipSync } from 'node:zlib';

const IN_DIR = process.argv[2];
const OUT    = process.argv[3];
if (!IN_DIR || !OUT) {
  console.error('usage: node pack-web.mjs <web_image_dir> <out.bin>');
  process.exit(1);
}

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js':   'application/javascript',
  '.mjs':  'application/javascript',
  '.css':  'text/css',
  '.json': 'application/json',
  '.svg':  'image/svg+xml',
  '.ico':  'image/x-icon',
  '.png':  'image/png',
  '.jpg':  'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif':  'image/gif',
  '.webp': 'image/webp',
  '.woff2':'font/woff2',
  '.woff': 'font/woff',
  '.ttf':  'font/ttf',
  '.txt':  'text/plain; charset=utf-8',
  '.webmanifest': 'application/manifest+json',
  '.map':  'application/json',
};
const GZIP_EXT = new Set(['.html', '.js', '.mjs', '.css', '.json', '.svg', '.ico', '.txt', '.webmanifest']);

const extOf  = (p) => { const i = p.lastIndexOf('.'); return i < 0 ? '' : p.slice(i).toLowerCase(); };
const mimeOf = (p) => MIME[extOf(p)] || 'application/octet-stream';

// Collect every file under IN_DIR.
const files = [];
(function walk(dir) {
  for (const name of readdirSync(dir)) {
    const full = join(dir, name);
    if (statSync(full).isDirectory()) walk(full);
    else files.push(full);
  }
})(IN_DIR);

const entries = [];
for (const full of files) {
  const path = '/' + relative(IN_DIR, full).split(sep).join('/');
  if (path.endsWith('.map')) continue;                 // never ship source maps
  const raw = readFileSync(full);
  let data = raw, gzip = false;
  if (GZIP_EXT.has(extOf(path))) {
    const gz = gzipSync(raw, { level: 9 });
    if (gz.length < raw.length) { data = gz; gzip = true; }
  }
  entries.push({ path, mime: mimeOf(path), gzip, data });
}
if (entries.length === 0) {
  console.error(`[pack-web] no files found under ${IN_DIR}`);
  process.exit(1);
}
entries.sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));

// Size the directory so we can assign absolute data offsets.
let dirSize = 0;
for (const e of entries) {
  e.pb = Buffer.from(e.path, 'utf8');
  e.mb = Buffer.from(e.mime, 'utf8');
  dirSize += 2 + e.pb.length + 1 + 2 + e.mb.length + 1 + 1 + 4 + 4;
}
const headerSize = 12 + dirSize;
let off = headerSize;
for (const e of entries) { e.off = off; off += e.data.length; }
const total = off;

const buf = Buffer.alloc(total);
let o = 0;
o += buf.write('WBND', o, 'ascii');
o = buf.writeUInt32LE(1, o);
o = buf.writeUInt32LE(entries.length, o);
for (const e of entries) {
  o = buf.writeUInt16LE(e.pb.length, o); o += e.pb.copy(buf, o); buf[o++] = 0;
  o = buf.writeUInt16LE(e.mb.length, o); o += e.mb.copy(buf, o); buf[o++] = 0;
  buf[o++] = e.gzip ? 1 : 0;
  o = buf.writeUInt32LE(e.off, o);
  o = buf.writeUInt32LE(e.data.length, o);
}
if (o !== headerSize) {
  console.error(`[pack-web] internal error: header wrote ${o}, expected ${headerSize}`);
  process.exit(1);
}
for (const e of entries) e.data.copy(buf, e.off);

mkdirSync(dirname(OUT), { recursive: true });
writeFileSync(OUT, buf);

const gz = entries.filter((e) => e.gzip).length;
console.log(`[pack-web] ${entries.length} files (${gz} gzipped) → ${OUT}  (${total} bytes)`);
