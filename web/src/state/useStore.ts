import { useCallback, useSyncExternalStore } from 'react';
import type { Store } from './store';

/**
 * Read one value out of a store, and re-render when that value changes.
 *
 * The selector must return something comparable with Object.is: a number, a
 * string, a boolean, or an object the store itself keeps stable. Returning a
 * fresh object from a selector would make every write look like a change, and
 * React would render until it gave up.
 *
 * This hook is for chrome state only. Nothing that changes every frame is read
 * through React; see the note on FrameState.
 */
export function useStoreValue<T extends object, V>(
  store: Store<T>,
  select: (state: T) => V,
): V {
  const subscribe = useCallback(
    (onChange: () => void) => store.subscribe(onChange),
    [store],
  );
  const snapshot = useCallback(() => select(store.get()), [store, select]);
  return useSyncExternalStore(subscribe, snapshot, snapshot);
}

const whole = <T>(state: T): T => state;

/**
 * Read the whole of a store.
 *
 * For a component that shows most of what a store holds, which the console
 * does. The state object is replaced on a write and kept between writes, so
 * this is a comparison by identity rather than a re-render on every write.
 */
export function useStoreState<T extends object>(store: Store<T>): T {
  return useStoreValue(store, whole);
}
