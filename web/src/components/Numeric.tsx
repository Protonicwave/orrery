import {
  decimal,
  MINUS,
  scientific,
  spoken,
  THIN_SPACE,
  TIMES,
  withSign,
} from '../format/number';
import styles from './Numeric.module.css';

export type Notation = 'decimal' | 'scientific' | 'signed';

export interface NumericProps {
  value: number;
  /** Digits after the point. For scientific notation, digits in the mantissa. */
  digits?: number;
  notation?: Notation;
  /** Set beside the figure, a weight lighter. */
  unit?: string;
  className?: string;
}

/**
 * One number, set the way this design system sets numbers.
 *
 * Every value in the interface goes through here. That is what makes the rules
 * hold everywhere at once rather than in the places somebody remembered: real
 * minus signs, thin spaces between digit groups, exponents raised rather than
 * spelled with an e, units a weight lighter, and tabular figures throughout.
 *
 * A value that a synthesiser would read wrongly carries a spoken form beside
 * the set one. "3.3×10⁻³" is four characters and a raised digit to a reader
 * and nonsense to a screen reader, so the set form is hidden from the
 * accessibility tree and a sentence is offered in its place.
 */
export function Numeric({
  value,
  digits,
  notation = 'decimal',
  unit,
  className,
}: NumericProps) {
  const classes =
    className === undefined ? styles.numeric : `${styles.numeric} ${className}`;
  const figure = set(value, notation, digits);
  const needsSpeech =
    unit !== undefined ||
    notation === 'scientific' ||
    (typeof figure === 'string' &&
      (figure.includes(MINUS) || figure.includes(THIN_SPACE)));

  if (!needsSpeech) return <span className={classes}>{figure}</span>;

  return (
    <span className={classes}>
      <span aria-hidden="true">
        {figure}
        {unit !== undefined && <span className="unit">{unit}</span>}
      </span>
      <span className="visually-hidden">
        {spoken(value, unit, notation === 'scientific')}
      </span>
    </span>
  );
}

function set(value: number, notation: Notation, digits: number | undefined) {
  if (notation === 'scientific') {
    const { mantissa, exponent } = scientific(value, digits ?? 1);
    if (exponent === '') return mantissa;
    return (
      <>
        {mantissa}
        {TIMES}10<sup className={styles.exponent}>{exponent}</sup>
      </>
    );
  }
  if (notation === 'signed') return withSign(value, digits ?? 2);
  return decimal(value, digits ?? 0);
}
