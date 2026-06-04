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

// Build the frontend into ../build/web_image; the path is overridable via the
// OUT_DIR env var when called from CMake. The built files are then packed
// (tools/pack-web.mjs) and embedded straight into the firmware image.
export default defineConfig(({ command }) => ({
  plugins: [vue(), trimPrimeicons],
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
