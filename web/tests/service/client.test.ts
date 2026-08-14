/**
 * Talking to the service, and what happens when there is nothing to talk to.
 *
 * Most of these are the unhappy cases on purpose. The happy one is a fetch and
 * a JSON parse and there is not much to get wrong in it; what decides whether
 * the instrument stays usable is that a service which is absent, slow, broken
 * or answering through something else all arrive as ordinary values rather than
 * as exceptions nobody catches.
 *
 * The module reads its address once, when it is loaded, so each case stubs the
 * environment and imports it again. That is the cost of the address being a
 * build-time setting, and it is worth paying: a page that had to be told at run
 * time where its service is would need somewhere to be told from.
 */

import { afterEach, describe, expect, it, vi } from 'vitest';

const ADDRESS = 'https://compute.example/api';

/** The module, loaded with a service configured or without one. */
async function load(service: string | undefined) {
  vi.resetModules();
  if (service === undefined) vi.stubEnv('VITE_ORRERY_SERVICE', '');
  else vi.stubEnv('VITE_ORRERY_SERVICE', service);
  return import('../../src/service/client');
}

function answering(status: number, body: unknown): void {
  vi.stubGlobal(
    'fetch',
    vi.fn(async () =>
      Promise.resolve(
        new Response(JSON.stringify(body), {
          status,
          headers: { 'Content-Type': 'application/json' },
        }),
      ),
    ),
  );
}

afterEach(() => {
  vi.unstubAllEnvs();
  vi.unstubAllGlobals();
});

describe('a build with no service', () => {
  it('says so rather than trying to fetch from nowhere', async () => {
    const client = await load(undefined);
    const fetching = vi.fn();
    vi.stubGlobal('fetch', fetching);

    expect(client.configured()).toBe(false);
    const answer = await client.capabilities();

    expect(answer.ok).toBe(false);
    if (answer.ok) return;
    expect(answer.problems[0]?.setting).toBe('service');
    expect(fetching).not.toHaveBeenCalled();
  });

  it('tells a watcher at once instead of leaving it waiting', async () => {
    const client = await load(undefined);
    const lost = vi.fn();
    const stop = client.watch('a-job', vi.fn(), lost);

    expect(lost).toHaveBeenCalledTimes(1);
    stop();
  });
});

describe('a service that answers', () => {
  it('hands back what it said', async () => {
    const client = await load(ADDRESS);
    answering(200, { queued: 2, workers: 1, accepting: true });

    const answer = await client.capabilities();
    expect(answer.ok).toBe(true);
    if (!answer.ok) return;
    expect(answer.value.queued).toBe(2);
    expect(fetch).toHaveBeenCalledWith(`${ADDRESS}/capabilities`, expect.anything());
  });

  it('submits the configuration as the one field the contract has', async () => {
    const client = await load(ADDRESS);
    answering(202, { job: { id: 'x' }, duplicate: false });

    await client.submit('[run]\ntimestep = 1\n');

    const call = vi.mocked(fetch).mock.calls[0];
    expect(call?.[0]).toBe(`${ADDRESS}/jobs`);
    expect(JSON.parse(String(call?.[1]?.body))).toEqual({
      configuration: '[run]\ntimestep = 1\n',
    });
  });

  it('reports a refusal in the words the service used', async () => {
    const client = await load(ADDRESS);
    answering(422, {
      problems: [{ setting: 'run.timestep', complaint: 'must be a positive number' }],
    });

    const answer = await client.submit('[run]\nsteps = 1\n');
    expect(answer.ok).toBe(false);
    if (answer.ok) return;
    expect(answer.problems).toEqual([
      { setting: 'run.timestep', complaint: 'must be a positive number' },
    ]);
  });

  it('does not read a proxy’s error page as a refusal', async () => {
    // A gateway answering instead of the service sends something that is not
    // the contract's shape. Reported as what it is rather than parsed hopefully.
    const client = await load(ADDRESS);
    answering(502, { message: 'upstream is unwell' });

    const answer = await client.capabilities();
    expect(answer.ok).toBe(false);
    if (answer.ok) return;
    expect(answer.problems[0]?.complaint).toContain('502');
  });

  it('treats a connection that fails as a service that is not there', async () => {
    const client = await load(ADDRESS);
    vi.stubGlobal(
      'fetch',
      vi.fn(() => Promise.reject(new TypeError('failed to fetch'))),
    );

    const answer = await client.capabilities();
    expect(answer.ok).toBe(false);
    if (answer.ok) return;
    expect(answer.problems[0]?.complaint).toContain('could not be reached');
  });
});

describe('where a result is fetched from', () => {
  it('is the service’s own address, not the site’s', async () => {
    const client = await load(ADDRESS);
    const job = {
      trajectory: '/jobs/abc/trajectory',
      diagnostics: '/jobs/abc/diagnostics',
    } as Parameters<typeof client.resultUrl>[0];

    expect(client.resultUrl(job)).toBe(`${ADDRESS}/jobs/abc/trajectory`);
    expect(client.diagnosticsUrl(job)).toBe(`${ADDRESS}/jobs/abc/diagnostics`);
  });

  it('is nothing at all until the run has produced one', async () => {
    const client = await load(ADDRESS);
    const job = { trajectory: null, diagnostics: null } as Parameters<
      typeof client.resultUrl
    >[0];

    expect(client.resultUrl(job)).toBeNull();
    expect(client.diagnosticsUrl(job)).toBeNull();
  });

  it('does not mind a trailing slash on the address', async () => {
    const client = await load(`${ADDRESS}/`);
    const job = {
      trajectory: '/jobs/abc/trajectory',
    } as Parameters<typeof client.resultUrl>[0];

    expect(client.resultUrl(job)).toBe(`${ADDRESS}/jobs/abc/trajectory`);
  });
});

describe('following a job', () => {
  it('falls back to asking when a socket will not open', async () => {
    const client = await load(ADDRESS);

    // A WebSocket that fails the way a proxy which does not pass them fails:
    // constructed, and then an error rather than an open.
    class Failing {
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      onmessage: (() => void) | null = null;
      close(): void {}
      constructor() {
        setTimeout(() => this.onerror?.(), 0);
      }
    }
    vi.stubGlobal('WebSocket', Failing);
    answering(200, {
      id: 'abc',
      state: 'done',
      progress: {},
      trajectory: '/jobs/abc/trajectory',
    });

    const updates: unknown[] = [];
    const stop = client.watch('abc', (job) => updates.push(job), vi.fn());

    // The socket errors on a timer, then the first poll answers.
    await new Promise((resolve) => setTimeout(resolve, 10));
    stop();

    expect(updates).toHaveLength(1);
    expect(fetch).toHaveBeenCalledWith(`${ADDRESS}/jobs/abc`, expect.anything());
  });

  it('stops following once the job cannot change again', async () => {
    const client = await load(ADDRESS);
    const closed = vi.fn();

    class Socket {
      onerror: (() => void) | null = null;
      onclose: (() => void) | null = null;
      onmessage: ((event: { data: string }) => void) | null = null;
      close = closed;
      constructor() {
        setTimeout(() => {
          this.onmessage?.({ data: JSON.stringify({ id: 'abc', state: 'done' }) });
        }, 0);
      }
    }
    vi.stubGlobal('WebSocket', Socket);

    const updates: unknown[] = [];
    client.watch('abc', (job) => updates.push(job), vi.fn());
    await new Promise((resolve) => setTimeout(resolve, 10));

    expect(updates).toHaveLength(1);
    // 1000 is a normal closure: the page is done with this job rather than
    // reporting a fault.
    expect(closed).toHaveBeenCalledWith(1000);
  });
});
