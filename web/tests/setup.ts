import { cleanup } from '@testing-library/react';
import { afterEach } from 'vitest';

// Testing Library registers this itself when the test globals are on. They are
// off here, because a test that has to import what it uses is a test that says
// what it uses.
afterEach(cleanup);
