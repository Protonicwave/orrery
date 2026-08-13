import { type ReactNode, useId } from 'react';
import styles from './Control.module.css';

export interface SliderProps {
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
  onChange,
}: SliderProps) {
  const id = useId();
  return (
    <div className={styles.control}>
      <label className={styles.name} htmlFor={id}>
        {name}
      </label>
      <input
        id={id}
        type="range"
        className={dear ? `${styles.slider} ${styles.dear}` : styles.slider}
        min={min}
        max={max}
        step={step}
        value={value}
        aria-valuetext={valueText}
        onChange={(event) => onChange(Number(event.target.value))}
      />
      <span className={styles.value}>{display}</span>
    </div>
  );
}

export interface SegmentedProps<T extends string> {
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
  onChange,
}: SegmentedProps<T>) {
  const id = useId();
  return (
    <div className={styles.control}>
      <span className={styles.name} id={id}>
        {name}
      </span>
      <div
        className={dear ? `${styles.segmented} ${styles.dear}` : styles.segmented}
        role="radiogroup"
        aria-labelledby={id}
      >
        {options.map((option) => (
          <label className={styles.option} key={option}>
            <input
              type="radio"
              name={name}
              value={option}
              checked={option === value}
              onChange={() => onChange(option)}
            />
            <span>{option}</span>
          </label>
        ))}
      </div>
      <span className={styles.value} />
    </div>
  );
}

export interface ToggleProps {
  label: string;
  pressed: boolean;
  onChange: (pressed: boolean) => void;
}

/** An on or off switch, drawn as a mark and a word rather than as a colour. */
export function Toggle({ label, pressed, onChange }: ToggleProps) {
  return (
    <button
      type="button"
      className={styles.toggle}
      aria-pressed={pressed}
      onClick={() => onChange(!pressed)}
    >
      {label}
    </button>
  );
}

export function Toggles({ children }: { children: ReactNode }) {
  return <div className={styles.toggles}>{children}</div>;
}
