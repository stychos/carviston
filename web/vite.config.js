import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

// App version string: 0.<config-schema>.<build>. The single source of truth is
// tools/gen-version.mjs, run by CMake at build time; it increments the counter
// and hands us the result via the APP_VERSION env var (set by web/build.sh from
// the generated app_version.txt), so the footer and the firmware always report
// the identical string.
//
// On `vite dev` (no CMake, no env) we fall back to computing the string
// read-only — same rules, but WITHOUT incrementing the counter, so dev serves
// don't burn build numbers.
function appVersion() {
  if (process.env.APP_VERSION) return process.env.APP_VERSION;

  const here = (p) => fileURLToPath(new URL(p, import.meta.url));
  let cfg = '?';
  try {
    const m = readFileSync(here('../main/app_config.c'), 'utf8')
      .match(/#define\s+CFG_VERSION\s+(\d+)/);
    if (m) cfg = m[1];
  } catch { /* leave '?' if the file moved — visible, not fatal */ }

  let savedCfg = null, build = 0;
  try {
    const parts = readFileSync(here('./.build_version'), 'utf8').trim().split(/\s+/);
    if (parts.length >= 2) { savedCfg = parts[0]; build = parseInt(parts[1], 10) || 0; }
  } catch { /* first build */ }
  if (savedCfg !== cfg) build = 0;
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
  // runtime API call (works offline / on the boot screen). The value comes from
  // tools/gen-version.mjs via APP_VERSION (see appVersion()).
  define: {
    __APP_VERSION__: JSON.stringify(appVersion()),
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
