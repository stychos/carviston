import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';

// Build the frontend into ../build/web_image so ESP-IDF's spiffs generator can
// pick it up. The path is overridable via OUT_DIR env var when called from CMake.
export default defineConfig({
  plugins: [vue()],
  build: {
    outDir: process.env.OUT_DIR || 'dist',
    emptyOutDir: true,
    target: 'es2020',
    cssCodeSplit: false,
    rollupOptions: {
      output: {
        // Single chunked bundle keeps SPIFFS file count modest.
        manualChunks: undefined,
        entryFileNames: 'assets/app.js',
        chunkFileNames: 'assets/[name].js',
        assetFileNames: (info) => {
          if (info.name?.endsWith('.css')) return 'assets/app.css';
          return 'assets/[name][extname]';
        },
      },
    },
  },
  server: {
    proxy: {
      '/api': { target: 'http://carviston.local', changeOrigin: true },
      '/ws':  { target: 'ws://carviston.local',   ws: true, changeOrigin: true },
    },
  },
});
