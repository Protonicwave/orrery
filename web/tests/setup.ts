import { cleanup } from '@testing-library/react';
import { afterEach } from 'vitest';

// Testing Library registers this itself when the test globals are on. They are
// off here, because a test that has to import what it uses is a test that says
// what it uses.
afterEach(cleanup);

// jsdom implements no layout, so it implements no ResizeObserver either. The
// sparklines watch their box with one, because a plot is drawn in device
// pixels and has to be redrawn when the box it is drawn into changes size. A
// stub that observes nothing is the honest stand-in: nothing here is ever
// resized, so nothing here should be called back.
if (!('ResizeObserver' in globalThis)) {
  globalThis.ResizeObserver = class {
    observe() {}
    unobserve() {}
    disconnect() {}
  } as unknown as typeof ResizeObserver;
}
