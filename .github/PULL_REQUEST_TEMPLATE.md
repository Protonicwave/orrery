## What changed

<!-- The change itself, in a few sentences. -->

## Why

<!-- The reason the change is worth making. If it implements a phase from the
     implementation plan, say which one. -->

## How it was verified

<!-- Commands run, tests added, and what they demonstrate. Name the test
     category: unit, property, validation or regression. -->

## Measured numbers

<!-- Required for any change that affects performance: before and after figures
     with the machine state and the benchmark method used to obtain them.
     Write "not applicable" if the change cannot affect performance. -->

## Architecture decision records

<!-- Links to any ADRs this pull request introduces, or "none". -->

## Definition of done

- [ ] Builds clean with Clang, GCC and MSVC, warnings as errors
- [ ] `clang-format` and `clang-tidy` pass with no diagnostics
- [ ] Address and undefined-behaviour sanitiser builds pass the full test suite
- [ ] New code is covered by tests in at least one of the four categories
- [ ] Public headers document purpose and rationale
- [ ] Any new non-obvious decision has an ADR
- [ ] Performance-affecting changes report before and after figures
- [ ] The README reflects reality and every claim in it is reproducible
- [ ] No AI attribution anywhere in the diff
- [ ] The phase status table in `docs/IMPLEMENTATION_PLAN.md` is updated
