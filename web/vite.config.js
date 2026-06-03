import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

// App version string: 0.<config-schema>.<build>.
//  - config-schema is the firmware's CFG_VERSION, the single source of truth in
//    main/app_config.c (CLAUDE.md requires bumping it on every config change).
//    We read it here rather than duplicate it, so the footer can never drift
//    from the actual on-device schema.
//  - build is a monotonic counter in an untracked file next to this config,
//    incremented once per PRODUCTION build (vite build) — not on dev serve — so
//    every flashed/OTA'd image carries a unique, increasing identifier.
function appVersion(isBuild) {
  const here = (p) => fileURLToPath(new URL(p, import.meta.url));

  let cfg = '?';
  try {
    const m = readFileSync(here('../main/app_config.c'), 'utf8')
      .match(/#define\s+CFG_VERSION\s+(\d+)/);
    if (m) cfg = m[1];
  } catch { /* leave '?' if the file moved — visible, not fatal */ }

  // The counter file records the config version it was counting under ("<cfg>
  // <build>") so build numbering restarts at 0 whenever CFG_VERSION bumps — a
  // schema change is a new minor line, so 0.7.42 → 0.8.1, not 0.8.43. An old
  // single-number file (no cfg recorded) is treated as a mismatch and resets.
  const counter = here('./.build_version');
  let savedCfg = null, build = 0;
  try {
    const parts = readFileSync(counter, 'utf8').trim().split(/\s+/);
    if (parts.length >= 2) { savedCfg = parts[0]; build = parseInt(parts[1], 10) || 0; }
  } catch { /* first build */ }
  if (savedCfg !== cfg) build = 0;   // config schema changed → restart build numbering
  if (isBuild) {
    build += 1;
    try { writeFileSync(counter, `${cfg} ${build}\n`); } catch { /* read-only FS: still show prior */ }
  }
  return `0.${cfg}.${build}`;
}

// Drop the legacy primeicons font formats (.eot/.ttf/.svg). Every browser this
// dashboard ever talks to supports woff2; .woff stays as a one-format fallback
// for older clients. Removing the url() references prevents Vite from emitting
// those font files as assets, saving ~510 KB in the embedded bundle (woff2
// alone is what the browser actually loads).
const trimPrimeicons = {
  name: 'trim-primeicons-formats',
  enforce: 'pre',
  transform(code, id) {
    if (!id.endsWith('primeicons.css')) return null;
    return code.replace(
      /@font-face\s*{[\s\S]*?}/,
      `@font-face {
    font-family: 'primeicons';
    font-display: block;
    src: url('./fonts/primeicons.woff2') format('woff2'),
         url('./fonts/primeicons.woff') format('woff');
    font-weight: normal;
    font-style: normal;
}`,
    );
  },
};

// Two-stage loader. PROBLEM: a single <script type="module"> for the ~211 KB
// (gzip) app bundle is a ONE-SHOT load — if the link drops mid-transfer the
// browser fires no retry and the UI hangs on the boot spinner forever. Over the
// heater's poor Wi-Fi that's the difference between "cached load works" and
// "uncached load shows nothing".
//
// FIX: ship a tiny inline bootstrap in index.html (stage 1 — always arrives
// with the HTML, nothing separate to fail) that pulls the CSS then the JS via
// fetch() with streaming + stall detection + infinite backoff retry (stage 2),
// then executes the JS from a blob URL. fetch() gives us what a <script> tag
// can't: abort a transfer that delivers no bytes for STALL_MS, retry with
// backoff, and surface live progress — so a flaky link just keeps trying until
// a clean window completes the download.
//
// Blob execution is safe ONLY because the bundle is a single self-contained
// chunk with no import.meta / dynamic import() (asserted by the asset checks);
// otherwise relative/meta-URL resolution would break under the blob: origin.
// Build-only: in dev (ctx.server) Vite serves native ESM with many sub-imports
// and HMR, which a blob import would shatter, so we leave dev untouched.
const BOOTSTRAP = (jsSrc, cssHref) =>
  '(function(){' +
  '"use strict";' +
  'var JS=' + JSON.stringify(jsSrc) + ',CSS=' + JSON.stringify(cssHref) + ';' +
  'var STALL_MS=12000,BACKOFF_MIN=1000,BACKOFF_MAX=8000,attempt=0;' +
  'function setStatus(t){var e=document.querySelector(".boot-status");if(e)e.textContent=t;}' +
  'function sleep(ms){return new Promise(function(r){setTimeout(r,ms);});}' +
  // Stream a URL to a Uint8Array, aborting if no chunk arrives within STALL_MS.
  'async function download(url,onBytes){' +
    'var ctrl=new AbortController(),timer;' +
    'function arm(){clearTimeout(timer);timer=setTimeout(function(){ctrl.abort();},STALL_MS);}' +
    'arm();var resp;' +
    'try{resp=await fetch(url,{signal:ctrl.signal,cache:"force-cache"});}' +
    'catch(e){clearTimeout(timer);throw e;}' +
    'if(!resp.ok){clearTimeout(timer);throw new Error("HTTP "+resp.status);}' +
    'if(!resp.body||!resp.body.getReader){clearTimeout(timer);return new Uint8Array(await resp.arrayBuffer());}' +
    'var reader=resp.body.getReader(),chunks=[],received=0;' +
    'for(;;){var r=await reader.read();if(r.done)break;chunks.push(r.value);received+=r.value.length;arm();if(onBytes)onBytes(received);}' +
    'clearTimeout(timer);' +
    'var buf=new Uint8Array(received),off=0,i;' +
    'for(i=0;i<chunks.length;i++){buf.set(chunks[i],off);off+=chunks[i].length;}' +
    'return buf;' +
  '}' +
  // Retry a download forever; decode to text. label drives the spinner caption.
  'async function fetchText(url,label){' +
    'for(;;){' +
      'try{var bytes=await download(url,function(n){setStatus(label+" \\u00B7 "+Math.round(n/1024)+" KB");});return new TextDecoder().decode(bytes);}' +
      'catch(e){attempt++;var wait=Math.min(BACKOFF_MIN*Math.pow(2,attempt-1),BACKOFF_MAX);setStatus("Connection lost \\u2014 retrying\\u2026");await sleep(wait);setStatus(label);}' +
    '}' +
  '}' +
  'async function boot(){' +
    // CSS first (small, completes fast, no contention) so the app paints styled;
    // then give the link's full capacity to the big JS.
    'if(CSS){var css=await fetchText(CSS,"Loading\\u2026");var st=document.createElement("style");st.textContent=css;document.head.appendChild(st);}' +
    'var js=await fetchText(JS,"Loading\\u2026");' +
    'setStatus("Starting\\u2026");' +
    'var url=URL.createObjectURL(new Blob([js],{type:"text/javascript"}));' +
    'try{await import(url);}catch(e){setStatus("Could not start the app");console.error(e);}' +
  '}' +
  'boot();' +
  '})();';

const twoStageLoader = {
  name: 'two-stage-loader',
  transformIndexHtml: {
    order: 'post',
    handler(html, ctx) {
      if (ctx.server) return html;   // dev: keep native ESM + HMR
      const js  = html.match(/<script[^>]*\bsrc="([^"]+\.js)"[^>]*><\/script>/);
      const css = html.match(/<link[^>]*\bhref="([^"]+\.css)"[^>]*>/);
      if (!js) return html;          // nothing to rewrite — leave as-is
      let out = html.replace(js[0], '');
      if (css) out = out.replace(css[0], '');
      const tag = '<script>' + BOOTSTRAP(js[1], css ? css[1] : '') + '</script>';
      return out.replace('</body>', tag + '</body>');
    },
  },
};

// Build the frontend into ../build/web_image; the path is overridable via the
// OUT_DIR env var when called from CMake. The built files are then packed
// (tools/pack-web.mjs) and embedded straight into the firmware image.
export default defineConfig(({ command }) => ({
  plugins: [vue(), trimPrimeicons, twoStageLoader],
  // Baked into the bundle at build time → shown instantly in the footer with no
  // runtime API call (works offline / on the boot screen). command==='build'
  // gates the per-build counter increment so dev serve doesn't bump it.
  define: {
    __APP_VERSION__: JSON.stringify(appVersion(command === 'build')),
  },
  build: {
    outDir: process.env.OUT_DIR || 'dist',
    emptyOutDir: true,
    target: 'es2020',
    // One CSS file keeps the asset list tiny. Everything else uses Vite's
    // default content-hashed names — the old SPIFFS 32-char object-name limit
    // is gone now that assets are embedded, and hashed names are what make the
    // device's immutable cache headers safe: a new build means new filenames,
    // so a stale asset can never be served after an OTA.
    cssCodeSplit: false,
    // The whole SPA is now ONE eager bundle (no defineAsyncComponent / dynamic
    // import). That's intentional: the UI is embedded in flash and served over
    // the LAN, so code-splitting bought no network win and a deferred chunk
    // could land late/unstyled in a constrained first-boot AP. ~210 KB gzip in
    // one file is the right trade here, so silence Vite's 500 KB chunk warning.
    chunkSizeWarningLimit: 1500,
  },
  server: {
    proxy: {
      '/api': { target: 'http://carviston.local', changeOrigin: true },
      '/ws':  { target: 'ws://carviston.local',   ws: true, changeOrigin: true },
    },
  },
}));
