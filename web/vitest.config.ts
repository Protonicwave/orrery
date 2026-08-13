import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';
import { readVersion } from './tools/version.ts';

export default defineConfig({
  plugins: [react()],
  define: {
    __ORRERY_VERSION__: JSON.stringify(readVersion()),
  },
  server: {
    // The tests read the repository's own configuration files, as the client
    // does. Same allowance, same reason.
    fs: { allow: ['..'] },
  },
  test: {
    environment: 'jsdom',
    setupFiles: ['tests/setup.ts'],
    include: ['tests/**/*.test.ts', 'tests/**/*.test.tsx'],
    css: true,
    restoreMocks: true,
  },
});
