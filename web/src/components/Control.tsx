import { type ReactNode, useId } from 'react';
import styles from './Control.module.css';

/**
 * What every control here shares.
 *
 * A control that cannot act is marked with `aria-disabled` rather than
 * `disabled`. The two are not the same thing: a disabled input is removed from
 * the tab order, so the sentence saying why it cannot act becomes something
 * only a pointer can reach, and the whole point of the note is that it is the
 * answer to "why is this greyed out". So the control is reachable, focusable,
 * announced as disabled, described by the note that explains it, and inert.
 */
interface Inert {
  /** Whether the control can act. A disabled one says why through describedBy. */
  disabled?: boolean;
  /** The identifier of the note explaining what a disabled control needs. */
  describedBy?: string;
}

export interface SliderProps extends Inert {
  name: string;
  value: number;
  min: number;
  max: number;
  step: number;
  /** The value as the interface sets it, shown to the right and spoken. */
  display: ReactNode;
  /** What a screen reader should say the value is. */
  valueText: string;
  /** Whether operating this control would need a new run. */
  dear?: boolean;
  onChange: (value: number) => void;
}

/**
 * A named slider with its value beside it.
 *
 * A range input rather than anything built here, because the native control
 * already answers to the arrow keys, Home and End, reports itself correctly to
 * a screen reader, and takes a formatted value through aria-valuetext. What is
 * replaced is its appearance and nothing else.
 */
export function Slider({
  name,
  value,
  min,
  max,
  step,
  display,
  valueText,
  dear = false,
  disabled = false,
  describedBy,
  onChange,
}: SliderProps) {
  const id = useId();
  const classes = [styles.slider, dear ? styles.dear : '', disabled ? styles.inert : '']
    .filter((part) => part !== '')
    .join(' ');

  return (
    <div className={styles.control}>
      <label className={styles.name} htmlFor={id}>
        {name}
      </label>
      <input
        id={id}
        type="range"
        className={classes}
        min={min}
        max={max}
        step={step}
        value={value}
        aria-valuetext={valueText}
        aria-disabled={disabled || undefined}
        aria-describedby={disabled ? describedBy : undefined}
        onChange={(event) => {
          if (disabled) return;
          onChange(Number(event.target.value));
        }}
      />
      <span className={styles.value}>{display}</span>
    </div>
  );
}

export interface SegmentedProps<T extends string> extends Inert {
  name: string;
  options: readonly T[];
  value: T;
  dear?: boolean;
  onChange: (value: T) => void;
}

/**
 * A choice of a few, laid out as one strip.
 *
 * Radio inputs under the appearance, so the arrow keys move within the group
 * and the group is one stop in the tab order, which is what a person expects
 * of a choice and what a screen reader announces.
 */
export function Segmented<T extends string>({
  name,
  options,
  value,
  dear = false,
  disabled = false,
  describedBy,
  onChange,
}: SegmentedProps<T>) {
  const id = useId();
  const classes = [
    styles.segmented,
    dear ? styles.dear : '',
    disabled ? styles.inert : '',
  ]
    .filter((part) => part !== '')
    .join(' ');

  return (
    <div className={styles.control}>
      <span className={styles.name} id={id}>
        {name}
      </span>
      <div
        className={classes}
        role="radiogroup"
        aria-labelledby={id}
        aria-disabled={disabled || undefined}
        aria-describedby={disabled ? describedBy : undefined}
      >
        {options.map((option) => (
          <label className={styles.option} key={option}>
            <input
              type="radio"
              name={name}
              value={option}
              checked={option === value}
              aria-disabled={disabled || undefined}
              onChange={() => {
                if (disabled) return;
                onChange(option);
              }}
            />
            <span>{option}</span>
          </label>
        ))}
      </div>
      <span className={styles.value} />
    </div>
  );
}

export interface ToggleProps extends Inert {
  label: string;
  pressed: boolean;
  onChange: (pressed: boolean) => void;
}

/** An on or off switch, drawn as a mark and a word rather than as a colour. */
export function Toggle({
  label,
  pressed,
  disabled = false,
  describedBy,
  onChange,
}: ToggleProps) {
  return (
    <button
      type="button"
      className={disabled ? `${styles.toggle} ${styles.inert}` : styles.toggle}
      aria-pressed={pressed}
      aria-disabled={disabled || undefined}
      aria-describedby={disabled ? describedBy : undefined}
      onClick={() => {
        if (disabled) return;
        onChange(!pressed);
      }}
    >
      {label}
    </button>
  );
}

export function Toggles({ children }: { children: ReactNode }) {
  return <div className={styles.toggles}>{children}</div>;
}
