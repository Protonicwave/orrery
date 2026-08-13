/**
 * How a number is set.
 *
 * Typography is doing measurement work here rather than decoration. A minus
 * sign is not a hyphen, an exponent is not the letter e, a thousands separator
 * is a thin space rather than a comma because a comma is a decimal point in
 * half of Europe, and every figure is tabular so that a value updating sixty
 * times a second cannot move the layout.
 *
 * These functions are pure and return strings or parts. Nothing here touches
 * the DOM; the Numeric component sets what they return.
 */

/** U+2212. The character an arithmetic sign is, rather than U+002D. */
export const MINUS = '−';

/** U+2009. Narrower than a word space, which is what a digit group wants. */
export const THIN_SPACE = ' ';

/** U+00D7. The multiplication sign, rather than a lower case letter x. */
export const TIMES = '×';

/** Replace the hyphen a formatter produces with the sign the value has. */
export function signed(text: string): string {
  return text.replace('-', MINUS);
}

/**
 * Group the integer part in threes with thin spaces: 60000 becomes 60 000.
 * The fractional part is left alone, which is what a reader expects and what
 * the C++ output does.
 */
export function group(text: string): string {
  const point = text.indexOf('.');
  const whole = point < 0 ? text : text.slice(0, point);
  const rest = point < 0 ? '' : text.slice(point);
  const sign = /^[+\-−]/.test(whole) ? whole[0] : '';
  const digits = sign === '' ? whole : whole.slice(1);
  return sign + digits.replace(/\B(?=(\d{3})+(?!\d))/g, THIN_SPACE) + rest;
}

/** A plain decimal, grouped and with a real minus sign. */
export function decimal(value: number, digits = 0): string {
  return group(signed(value.toFixed(digits)));
}

/**
 * A value that is read as a change rather than a quantity, so the sign is
 * always shown: the exposure readout is +1.60 or the value it is not.
 */
export function withSign(value: number, digits = 2): string {
  const sign = value < 0 ? MINUS : '+';
  return sign + Math.abs(value).toFixed(digits);
}

/** A number split for setting as a mantissa and a superscript exponent. */
export interface Scientific {
  readonly mantissa: string;
  readonly exponent: string;
}

/**
 * Split a value into 3.3 and −3, for setting as 3.3×10⁻³.
 *
 * Zero has no exponent and is returned as itself, because 0×10⁰ is a true
 * statement that no one wants to read.
 */
export function scientific(value: number, digits = 1): Scientific {
  if (value === 0) return { mantissa: '0', exponent: '' };

  const exponent = Math.floor(Math.log10(Math.abs(value)));
  const mantissa = value / 10 ** exponent;

  // Rounding the mantissa can carry it to ten, and 10×10⁻³ is not scientific
  // notation. Carry the exponent instead.
  const rounded = Number(mantissa.toFixed(digits));
  if (Math.abs(rounded) >= 10) {
    return {
      mantissa: signed((rounded / 10).toFixed(digits)),
      exponent: signed(String(exponent + 1)),
    };
  }
  return {
    mantissa: signed(rounded.toFixed(digits)),
    exponent: signed(String(exponent)),
  };
}

/**
 * A value split into the parts a readout writes into the document.
 *
 * The Numeric component sets a value that changes when a person changes
 * something. A diagnostic's value changes ten times a second as the run plays,
 * and rendering it would put four components on the React path at that rate to
 * move four numbers. So it is written into elements directly, and this is the
 * shape those elements are written from: the same typographic rules, the same
 * spoken form beside the set one, and no component allowed to invent either.
 */
export interface Reading {
  /** The figure itself, or the mantissa where there is an exponent. */
  readonly figure: string;
  /** The exponent, or empty where the value is not set in scientific notation. */
  readonly exponent: string;
  /** What a synthesiser should say instead. */
  readonly spoken: string;
}

/** A value set as a mantissa and a raised exponent: 3.3×10⁻³. */
export function setScientific(value: number, digits = 1): Reading {
  const { mantissa, exponent } = scientific(value, digits);
  return { figure: mantissa, exponent, spoken: spoken(value, undefined, true) };
}

/** A value set as a plain decimal, grouped and with a real minus sign. */
export function setDecimal(value: number, digits = 0, unit?: string): Reading {
  return {
    figure: decimal(value, digits) + (unit === undefined ? '' : unit),
    exponent: '',
    spoken: spoken(value, unit),
  };
}

/**
 * What a screen reader should say. A value set as 3.3×10⁻³ is three characters
 * and a raised digit to a sighted reader and nonsense to a synthesiser, so
 * every value carries a spoken form beside the set one.
 *
 * The spoken form follows the set one: a value the interface has chosen to set
 * in scientific notation is spoken in scientific notation, whatever its
 * magnitude, so that what is heard and what is seen are the same statement.
 */
export function spoken(value: number, unit?: string, asScientific = false): string {
  const magnitude = Math.abs(value);
  let words: string;
  if (value !== 0 && (asScientific || magnitude < 1e-3 || magnitude >= 1e6)) {
    const { mantissa, exponent } = scientific(value);
    words = `${mantissa.replace(MINUS, 'minus ')} times ten to the ${exponent.replace(
      MINUS,
      'minus ',
    )}`;
  } else {
    words = String(value).replace('-', 'minus ');
  }
  return unit === undefined ? words : `${words} ${unit}`;
}
