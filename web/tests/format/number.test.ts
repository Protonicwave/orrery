import { describe, expect, it } from 'vitest';
import {
  decimal,
  group,
  MINUS,
  scientific,
  signed,
  spoken,
  THIN_SPACE,
  withSign,
} from '../../src/format/number';

describe('grouping', () => {
  it('separates thousands with a thin space rather than a comma', () => {
    expect(group('60000')).toBe(`60${THIN_SPACE}000`);
    expect(group('2097152')).toBe(`2${THIN_SPACE}097${THIN_SPACE}152`);
  });

  it('leaves three digits and fewer alone', () => {
    expect(group('999')).toBe('999');
  });

  it('groups the whole part only', () => {
    expect(group('12345.6789')).toBe(`12${THIN_SPACE}345.6789`);
  });

  it('keeps a sign outside the grouping', () => {
    expect(group(`${MINUS}12345`)).toBe(`${MINUS}12${THIN_SPACE}345`);
  });
});

describe('signs', () => {
  it('sets a negative with a minus sign and not a hyphen', () => {
    expect(signed('-3')).toBe(`${MINUS}3`);
    expect(decimal(-1234, 0)).toBe(`${MINUS}1${THIN_SPACE}234`);
  });

  it('always shows the sign of a value read as a change', () => {
    expect(withSign(1.6)).toBe('+1.60');
    expect(withSign(-1.6)).toBe(`${MINUS}1.60`);
    expect(withSign(0)).toBe('+0.00');
  });
});

describe('scientific notation', () => {
  it('splits a value into a mantissa and an exponent', () => {
    expect(scientific(3.3e-3)).toEqual({ mantissa: '3.3', exponent: `${MINUS}3` });
    expect(scientific(1.2e-5)).toEqual({ mantissa: '1.2', exponent: `${MINUS}5` });
    expect(scientific(2781)).toEqual({ mantissa: '2.8', exponent: '3' });
  });

  it('carries the exponent rather than writing a mantissa of ten', () => {
    expect(scientific(9.99e-4)).toEqual({ mantissa: '1.0', exponent: `${MINUS}3` });
  });

  it('leaves zero as itself', () => {
    expect(scientific(0)).toEqual({ mantissa: '0', exponent: '' });
  });

  it('keeps the sign of the value on the mantissa', () => {
    expect(scientific(-3.3e-3).mantissa).toBe(`${MINUS}3.3`);
  });
});

describe('the spoken form', () => {
  it('says what a set exponent means', () => {
    expect(spoken(3.3e-5)).toBe('3.3 times ten to the minus 5');
  });

  it('names the unit', () => {
    expect(spoken(20.4, 'ms')).toBe('20.4 ms');
  });

  it('says minus rather than reading a hyphen', () => {
    expect(spoken(-4)).toBe('minus 4');
  });
});
