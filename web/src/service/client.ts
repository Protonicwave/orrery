/**
 * The client's half of the compute service.
 *
 * Everything that crosses the boundary is described by `contract.ts`, which is
 * generated from the service's own models, so nothing here restates what a job
 * or a rejection is. What is here is the four things a page does with the
 * service: ask what it will take, submit a run, follow one, and fetch what came
 * out.
 *
 * The whole module is written so that a service which is not there is an
 * ordinary answer rather than an exception in the console. A page that cannot
 * reach it shows the gallery and says so, which is ADR-0054, and that is only
 * possible if "unreachable" arrives as a value.
 */

import type { Accepted, Capabilities, Job, Problem, Rejection } from './contract';

/**
 * Where the service is, or an empty string if this build has none.
 *
 * A build-time setting rather than something discovered, because the site is
 * published to GitHub Pages and the service is not, so the two have different
 * origins and one of them may be absent. A build with no service configured is
 * a perfectly good build: the instrument plays the published gallery, and the
 * solver tier says that computing a new run needs a service this page has not
 * been given.
 */
export const SERVICE = (import.meta.env.VITE_ORRERY_SERVICE ?? '').replace(/\/$/, '');

/** Whether this build was given a service to talk to at all. */
export function configured(): boolean {
  return SERVICE !== '';
}

/** What came back: the thing asked for, or a reason it did not. */
export type Answer<T> =
  | { readonly ok: true; readonly value: T }
  | { readonly ok: false; readonly problems: readonly Problem[] };

function refused(complaint: string): Answer<never> {
  return { ok: false, problems: [{ setting: 'service', complaint }] };
}

/**
 * How long a request may take before it is treated as an unreachable service.
 *
 * Five seconds. The service answers every one of these from one or two short
 * statements, so anything slower is a network that is not going to produce an
 * answer worth waiting for, and the page has something to show in the meantime.
 */
const TIMEOUT_MS = 5000;

async function ask<T>(path: string, options?: RequestInit): Promise<Answer<T>> {
  if (!configured()) {
    return refused('this build of the client was not given a compute service');
  }

  try {
    const response = await fetch(`${SERVICE}${path}`, {
      ...options,
      signal: AbortSignal.timeout(TIMEOUT_MS),
    });

    if (response.ok) return { ok: true, value: (await response.json()) as T };

    // The service reports every objection at once, in one shape, whether it is
    // refusing the configuration or refusing to take anything at all. A body
    // that is not that shape is a proxy or a gateway answering instead of the
    // service, and it is reported as what it is rather than parsed hopefully.
    const body: unknown = await response.json().catch(() => null);
    const problems = (body as Rejection | null)?.problems;
    if (Array.isArray(problems) && problems.length > 0) {
      return { ok: false, problems };
    }
    return refused(`the service answered ${response.status}`);
  } catch (error) {
    return refused(
      error instanceof DOMException && error.name === 'TimeoutError'
        ? 'the service did not answer'
        : 'the service could not be reached',
    );
  }
}

/** What the service will take, right now. */
export function capabilities(): Promise<Answer<Capabilities>> {
  return ask<Capabilities>('/capabilities');
}

/** Submit a configuration. */
export function submit(configuration: string): Promise<Answer<Accepted>> {
  return ask<Accepted>('/jobs', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ configuration }),
  });
}

/** One job, as it stands. */
export function job(id: string): Promise<Answer<Job>> {
  return ask<Job>(`/jobs/${id}`);
}

/** Where a finished job's trajectory is fetched from. */
export function resultUrl(finished: Job): string | null {
  return finished.trajectory === null ? null : `${SERVICE}${finished.trajectory}`;
}

/** And its diagnostics. */
export function diagnosticsUrl(finished: Job): string | null {
  return finished.diagnostics === null ? null : `${SERVICE}${finished.diagnostics}`;
}

/** A job is finished when it will not change again. */
export function settled(state: Job['state']): boolean {
  return state === 'done' || state === 'failed';
}

/**
 * How often a job is asked about when there is no socket.
 *
 * Two seconds. The service reports progress about twenty times over a run, so
 * asking faster than this learns nothing new; asking much slower would make a
 * queue position take longer to update than it takes to change.
 */
const POLL_MS = 2000;

/**
 * Follow a job until it is finished, and stop.
 *
 * A WebSocket first, and polling if the socket will not open or drops before
 * the job is done. Both deliver the same `Job`, because the service sends the
 * whole job over the socket rather than a smaller update: falling back changes
 * how often the page learns things and not what it learns, so nothing
 * downstream has to know which of the two is running.
 *
 * Returns the function that stops following. Calling it is not optional: a
 * socket left open after the console has moved on is a connection nobody reads.
 */
export function watch(
  id: string,
  onUpdate: (job: Job) => void,
  onLost: (problems: readonly Problem[]) => void,
): () => void {
  let stopped = false;
  let socket: WebSocket | null = null;
  let timer: ReturnType<typeof setTimeout> | null = null;

  const stop = (): void => {
    stopped = true;
    if (timer !== null) clearTimeout(timer);
    // 1000 is a normal closure: the page is done with this job rather than
    // reporting a fault, and a socket closed any other way is logged as an
    // error by some browsers.
    socket?.close(1000);
    socket = null;
  };

  const deliver = (next: Job): void => {
    onUpdate(next);
    if (settled(next.state)) stop();
  };

  const poll = async (): Promise<void> => {
    if (stopped) return;
    const answer = await job(id);
    if (stopped) return;

    if (!answer.ok) {
      onLost(answer.problems);
      return;
    }
    deliver(answer.value);
    if (!stopped) timer = setTimeout(() => void poll(), POLL_MS);
  };

  if (!configured()) {
    onLost([
      {
        setting: 'service',
        complaint: 'this build of the client was not given a compute service',
      },
    ]);
    return stop;
  }

  try {
    const address = `${SERVICE.replace(/^http/, 'ws')}/jobs/${id}/progress`;
    socket = new WebSocket(address);
    socket.onmessage = (event) => {
      deliver(JSON.parse(String(event.data)) as Job);
    };
    socket.onerror = () => {
      // Not reported. A socket that will not open is a proxy that does not pass
      // them or a network that does not like them, and neither is something the
      // person watching a run can do anything about: what they want is for the
      // progress to keep moving, which polling does.
      socket = null;
      void poll();
    };
    socket.onclose = (event) => {
      socket = null;
      // A close the page asked for is not a fallback.
      if (!stopped && event.code !== 1000) void poll();
    };
  } catch {
    void poll();
  }

  return stop;
}
