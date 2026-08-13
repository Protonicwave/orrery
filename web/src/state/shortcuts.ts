/**
 * The native viewer's keys, reachable from anywhere on the page.
 *
 * `orrery-view` has one surface and six keys, and `src/viz/viewer_window.cpp`
 * reads them from the window: space to pause, `r` to reframe, `-` and `=` for
 * the exposure. A browser has no such thing as the window having focus, so the
 * same keys pressed by someone who has not clicked the plate first would do
 * nothing at all, and an instrument whose controls depend on which invisible
 * element was last clicked is an instrument that appears to be broken.
 *
 * So the same keys are read here as well. The plate keeps its own handler,
 * because when it has focus it also answers the arrow keys and the page must
 * not scroll under them; this one covers the rest of the document.
 *
 * What it does not cover is anything that already means something. A key
 * pressed inside a control belongs to that control: space presses a button and
 * moves a range input, `-` types a minus sign, and taking those away to drive a
 * transport would break the controls the transport is made of.
 */

import { useEffect } from 'react';

export interface ViewerShortcuts {
  onPlayPause: () => void;
  onReframe: () => void;
  /** A multiple applied to the exposure: above one is brighter. */
  onExposure: (factor: number) => void;
}

/** What the native viewer's `=` and `-` do to the exposure, per press. */
const EXPOSURE_STEP = 1.03;

/** Elements that answer these keys themselves. */
const OWNS_ITS_KEYS = new Set([
  'INPUT',
  'TEXTAREA',
  'SELECT',
  'BUTTON',
  'A',
  'CANVAS',
  'SUMMARY',
]);

function claimed(target: EventTarget | null): boolean {
  if (!(target instanceof HTMLElement)) return false;
  return OWNS_ITS_KEYS.has(target.tagName) || target.isContentEditable;
}

export function useViewerShortcuts(shortcuts: ViewerShortcuts): void {
  const { onPlayPause, onReframe, onExposure } = shortcuts;

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent): void => {
      if (event.ctrlKey || event.altKey || event.metaKey) return;
      if (claimed(event.target)) return;

      switch (event.key) {
        case ' ':
          onPlayPause();
          break;
        case 'r':
          onReframe();
          break;
        case '-':
          onExposure(1 / EXPOSURE_STEP);
          break;
        case '=':
        case '+':
          onExposure(EXPOSURE_STEP);
          break;
        default:
          return;
      }
      // Space scrolls a document and the arrow keys move it. Neither is what
      // was meant by a key that has just moved the instrument.
      event.preventDefault();
    };

    window.addEventListener('keydown', onKeyDown);
    return () => {
      window.removeEventListener('keydown', onKeyDown);
    };
  }, [onPlayPause, onReframe, onExposure]);
}
