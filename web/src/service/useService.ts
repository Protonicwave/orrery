/**
 * The compute service, as a page sees it.
 *
 * Three things at once: what the service will take, the run this tab has
 * submitted, and why the last thing that was refused was refused. They are one
 * hook because they are one conversation, and because the console has to show
 * all three in the same corner of the screen.
 *
 * Everything here is chrome state. A job changes about twenty times over
 * several minutes, which is a rate React is for; nothing on this path touches
 * the render loop.
 */

import { useCallback, useEffect, useRef, useState } from 'react';
import {
  capabilities as askCapabilities,
  configured,
  submit as sendSubmission,
  settled,
  watch,
} from './client';
import type { Capabilities, Job, Problem } from './contract';

/**
 * How often the service is asked what it will take.
 *
 * Half a minute. What changes in that answer is whether a worker is alive and
 * how long the queue is, and both are things somebody deciding whether to
 * submit wants to be roughly right rather than exact. Asking on every render
 * would be a request per slider movement.
 */
const REFRESH_MS = 30_000;

export interface ServiceView {
  /** What the service will take, or null while that is not known. */
  readonly capabilities: Capabilities | null;
  /** Empty when the service answered. Otherwise why it did not. */
  readonly unreachable: string;
  /** The run this tab submitted, from acceptance to done or failed. */
  readonly job: Job | null;
  /** Why the last submission was refused, in the service's own words. */
  readonly problems: readonly Problem[];
  /** True between pressing the button and the service answering. */
  readonly busy: boolean;
  submit: (configuration: string) => void;
  /** Forget the job and any refusal, and go back to the published run. */
  dismiss: () => void;
}

export function useService(): ServiceView {
  const [capabilities, setCapabilities] = useState<Capabilities | null>(null);
  const [unreachable, setUnreachable] = useState(
    configured() ? '' : 'this build of the client was not given a compute service',
  );
  const [job, setJob] = useState<Job | null>(null);
  const [problems, setProblems] = useState<readonly Problem[]>([]);
  const [busy, setBusy] = useState(false);

  // Whether this hook is still mounted, so that an answer arriving after the
  // page has moved on is dropped rather than setting state on nothing.
  const mounted = useRef(true);
  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
    };
  }, []);

  const refresh = useCallback(async (): Promise<void> => {
    const answer = await askCapabilities();
    if (!mounted.current) return;
    if (answer.ok) {
      setCapabilities(answer.value);
      setUnreachable('');
    } else {
      // The capabilities are kept rather than cleared. A service that has
      // stopped answering was reachable a moment ago, and what it said then is
      // still the best description of what it would take; the console shows the
      // ceilings and says it cannot be reached.
      setUnreachable(answer.problems[0]?.complaint ?? 'the service did not answer');
    }
  }, []);

  useEffect(() => {
    void refresh();
    const timer = setInterval(() => void refresh(), REFRESH_MS);
    return () => {
      clearInterval(timer);
    };
  }, [refresh]);

  // Following the job. The dependency is the identifier rather than the job, so
  // that an update delivered by the socket does not close and reopen it.
  const identifier = job?.id ?? null;
  const finished = useRef(false);

  useEffect(() => {
    if (identifier === null) return;
    finished.current = false;

    const stop = watch(
      identifier,
      (next) => {
        setJob(next);
        if (settled(next.state) && !finished.current) {
          finished.current = true;
          // A finished run is a new measurement, and if it was the only thing
          // in the queue the service is now idle. Both are worth asking about.
          void refresh();
        }
      },
      (lost) => {
        setUnreachable(lost[0]?.complaint ?? 'the service stopped answering');
      },
    );
    return stop;
  }, [identifier, refresh]);

  const submit = useCallback(
    (configuration: string) => {
      setBusy(true);
      setProblems([]);
      void sendSubmission(configuration).then((answer) => {
        setBusy(false);
        if (!answer.ok) {
          setProblems(answer.problems);
          return;
        }
        setJob(answer.value.job);
        void refresh();
      });
    },
    [refresh],
  );

  const dismiss = useCallback(() => {
    setJob(null);
    setProblems([]);
  }, []);

  return { capabilities, unreachable, job, problems, busy, submit, dismiss };
}
