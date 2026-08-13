import { describe, expect, it, vi } from 'vitest';
import {
  createChromeState,
  createFrameState,
  createStore,
} from '../../src/state/store';

describe('the store', () => {
  it('holds the state it was given', () => {
    expect(createStore({ a: 1 }).get()).toEqual({ a: 1 });
  });

  it('merges a patch and tells its subscribers', () => {
    const store = createStore({ a: 1, b: 'x' });
    const listener = vi.fn();
    store.subscribe(listener);

    store.set({ a: 2 });

    expect(store.get()).toEqual({ a: 2, b: 'x' });
    expect(listener).toHaveBeenCalledWith({ a: 2, b: 'x' });
  });

  it('accepts a function of the current state', () => {
    const store = createStore({ a: 1 });
    store.set((state) => ({ a: state.a + 1 }));
    expect(store.get().a).toBe(2);
  });

  it('tells nobody about a write that changes nothing', () => {
    const store = createStore({ a: 1 });
    const listener = vi.fn();
    store.subscribe(listener);

    store.set({ a: 1 });

    expect(listener).not.toHaveBeenCalled();
  });

  it('keeps the state object between writes, so it compares by identity', () => {
    const store = createStore({ a: 1 });
    const before = store.get();
    store.set({ a: 1 });
    expect(store.get()).toBe(before);
    store.set({ a: 2 });
    expect(store.get()).not.toBe(before);
  });

  it('stops telling a subscriber that has unsubscribed', () => {
    const store = createStore({ a: 1 });
    const listener = vi.fn();
    const stop = store.subscribe(listener);

    stop();
    store.set({ a: 2 });

    expect(listener).not.toHaveBeenCalled();
  });

  it('survives a subscriber unsubscribing while it is being told', () => {
    const store = createStore({ a: 1 });
    const second = vi.fn();
    const stop = store.subscribe(() => stop());
    store.subscribe(second);

    expect(() => store.set({ a: 2 })).not.toThrow();
    expect(second).toHaveBeenCalledOnce();
  });
});

describe('chrome state', () => {
  it('opens showing the configuration it was given rather than a default', () => {
    const chrome = createChromeState({
      count: 60000,
      softening: 0.12,
      integrator: 'velocity-verlet',
    });
    expect(chrome.get().requestedCount).toBe(60000);
    expect(chrome.get().requestedSoftening).toBe(0.12);
  });
});

describe('frame state', () => {
  it('is a plain mutable record, because a listener on it would be a cost per frame', () => {
    const frame = createFrameState();
    frame.time = 1.5;
    expect(frame.time).toBe(1.5);
    expect(Object.keys(frame)).toEqual(['time', 'step', 'framesPerSecond', 'stepTime']);
  });
});
