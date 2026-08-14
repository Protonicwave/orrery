import { fileURLToPath } from 'node:url';
import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';
import { readVersion } from './tools/version.ts';

/** An entry of the site, by the path of the page that starts it. */
const page = (path: string): string => fileURLToPath(new URL(path, import.meta.url));

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

    // Seven pages, two of which run a script: the instrument and the editor.
    // The five under method/ carry no script tag, so nothing links them to the
    // client's bundle and the build emits them as markup and a stylesheet.
    // ADR-0050.
    rollupOptions: {
      input: {
        instrument: page('index.html'),
        editor: page('editor/index.html'),
        method: page('method/index.html'),
        demonstration: page('method/demonstration/index.html'),
        validation: page('method/validation/index.html'),
        performance: page('method/performance/index.html'),
        solvers: page('method/solvers/index.html'),
      },
    },
  },
  css: {
    modules: {
      // Class names are read in the browser's inspector more often than they
      // are read anywhere else, so they keep the name they were written with.
      generateScopedName: '[local]_[hash:base64:5]',
    },
  },
});
