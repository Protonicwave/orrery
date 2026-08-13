import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';
import { readVersion } from './tools/version.ts';

// The client is published beside the documentation site rather than in place of
// it, so every asset is addressed from this prefix. ADR-0045.
const base = process.env.ORRERY_BASE ?? '/orrery/instrument/';

export default defineConfig({
  base,
  plugins: [react()],
  define: {
    __ORRERY_VERSION__: JSON.stringify(readVersion()),
  },
  server: {
    // The client reads the repository's own configuration files, which sit
    // above this directory. Nothing else outside it is served.
    fs: { allow: ['..'] },
  },
  build: {
    // The budget in tools/budget.mjs is stated in gzipped bytes, so the report
    // the build prints has to be in the same units to be worth reading.
    reportCompressedSize: true,
    target: 'es2022',
    assetsInlineLimit: 0,
  },
  css: {
    modules: {
      // Class names are read in the browser's inspector more often than they
      // are read anywhere else, so they keep the name they were written with.
      generateScopedName: '[local]_[hash:base64:5]',
    },
  },
});
