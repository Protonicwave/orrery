import { render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { App } from '../src/App';
import { collision } from '../src/config/run';
import { decimal } from '../src/format/number';

/**
 * Testing Library folds every run of whitespace into one ordinary space before
 * it compares, which would let a thin space between digit groups pass as a
 * word space. These queries compare the text as it stands.
 */
function set(text: string) {
  return (_: string, element: Element | null) => element?.textContent === text;
}

/**
 * The shell states what the file states.
 *
 * Not a snapshot. Each of these reads a figure out of the configuration and
 * then requires the interface to be showing that figure, so the test fails
 * when the two disagree rather than when the layout moves.
 */
describe('the instrument', () => {
  it('shows the run it has loaded', () => {
    render(<App />);
    expect(screen.getByText('examples/collision.orrery')).toBeDefined();
    expect(screen.getByText(collision.name)).toBeDefined();
  });

  it('shows the particle count the configuration asks for', () => {
    render(<App />);
    expect(screen.getAllByText(set(decimal(collision.count))).length).toBeGreaterThan(
      0,
    );
  });

  it('shows the model time the timestep and the step count come to', () => {
    render(<App />);
    expect(collision.modelTime).toBe(collision.steps * collision.timestep);
    expect(
      screen.getAllByText(set(decimal(collision.modelTime, 3))).length,
    ).toBeGreaterThan(0);
  });

  it('names its landmarks, so a reader can move between them', () => {
    render(<App />);
    expect(screen.getByRole('banner')).toBeDefined();
    expect(screen.getByRole('complementary', { name: 'Run data' })).toBeDefined();
    expect(screen.getByRole('navigation', { name: 'Sections' })).toBeDefined();
  });

  it('offers every control to the keyboard', () => {
    render(<App />);
    for (const name of ['Exposure', 'Sprite radius', 'Softening', 'Bodies']) {
      expect(screen.getByRole('slider', { name })).toBeDefined();
    }
    expect(screen.getByRole('radiogroup', { name: 'Tone curve' })).toBeDefined();
    expect(screen.getByRole('button', { name: 'Trails' })).toBeDefined();
  });

  it('offers the transport, and says a new run is not reachable yet', () => {
    render(<App />);
    // Playback is a control now that there is a trajectory to play. Submitting
    // a run is still not, and still says so rather than pretending.
    expect(
      screen.getByRole('button', { name: 'Play' }).getAttribute('aria-pressed'),
    ).toBe('false');
    expect(
      screen.getByRole('button', { name: /Recompute/ }).getAttribute('aria-disabled'),
    ).toBe('true');
  });

  /**
   * This environment has neither a Worker nor a graphics context, which is the
   * same position a browser too old for either is in. What it must not do is
   * show an empty black rectangle, because a plate with nothing on it and a
   * renderer that failed look identical and only one of them should.
   */
  it('says what it cannot do rather than showing an empty plate', async () => {
    render(<App />);
    expect(await screen.findByText(/will not draw the plate|no Worker/)).toBeDefined();
  });
});
