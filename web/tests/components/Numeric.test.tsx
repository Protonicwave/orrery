import { render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { Numeric } from '../../src/components/Numeric';
import { MINUS, THIN_SPACE } from '../../src/format/number';

describe('a number, set', () => {
  it('groups digits with thin spaces', () => {
    const { container } = render(<Numeric value={60000} />);
    expect(container.textContent).toContain(`60${THIN_SPACE}000`);
  });

  it('raises the exponent rather than spelling it with an e', () => {
    const { container } = render(<Numeric value={3.3e-3} notation="scientific" />);
    const exponent = container.querySelector('sup');
    expect(exponent?.textContent).toBe(`${MINUS}3`);
    expect(container.textContent).not.toContain('e-3');
  });

  it('sets the unit beside the figure rather than inside it', () => {
    const { container } = render(<Numeric value={20.4} digits={1} unit="ms" />);
    expect(container.querySelector('.unit')?.textContent).toBe('ms');
  });

  it('offers a spoken form of a value a synthesiser would read wrongly', () => {
    render(<Numeric value={3.3e-3} notation="scientific" />);
    expect(screen.getByText('3.3 times ten to the minus 3')).toBeDefined();
  });

  it('hides the set form from the accessibility tree when it offers a spoken one', () => {
    const { container } = render(<Numeric value={1.2e-5} notation="scientific" />);
    expect(container.querySelector('[aria-hidden="true"]')).not.toBeNull();
  });

  it('leaves a plain figure alone, with nothing hidden and nothing spoken', () => {
    const { container } = render(<Numeric value={42} />);
    expect(container.textContent).toBe('42');
    expect(container.querySelector('[aria-hidden="true"]')).toBeNull();
  });
});
