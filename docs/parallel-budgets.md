# Parallel global budgets and cancellation (M5.4)

**Status:** Accepted (serial-fallback contract + regression proof)
**Scope:** M5.4 / GitHub issue #58

## Contract

Global early-stop semantics — match budgets, first-hit completion, cooperative
cancellation — are order-sensitive: the correct result is the serial traversal's
**ordered prefix**, and any parallel evaluation must retire work in index order
to produce it. The M5.1 queue completes tasks out of order and returns the
completed in-index prefix on cancellation, which can drop slower earlier tasks;
it is therefore **not** a valid engine for budgeted searches.

The rule is exclusive and total:

- The parallel merge (M5.2) runs **only** for exhaustive, unlimited
  (`max_matches == 0`), unobserved (`stats == nullptr`), uncancelled
  (`!should_cancel`) searches. Every parallel task runs to completion.
- **All** other semantics fall back to the serial path:
  - `max_matches != 0` (global match budget / ordered prefix),
  - `objective` of `FirstHit` or `OrderedPrefix`,
  - a set `should_cancel` hook,
  - a non-null `SearchStats*` observer.

## Consequences

- **Global match budgets:** `max_matches=K` with any `threads` returns exactly
  the serial K-prefix — the first K matches in (file_id, start, end, captures)
  order. No extra results, no lost earlier matches.
- **First-hit completion:** `FirstHit` with any `threads` returns the single
  first ordered match, identical to serial. CLI `--quiet` maps to `FirstHit`
  (`src/cli.cpp`: `core_opt.objective`), so quiet exit status is unaffected by
  `threads` and never scans unrelated work after the proven hit — the serial
  FirstHit path stops at the first ordered result by construction.
- **Cancellation propagation:** a set `should_cancel` forces serial, where the
  hook is consulted at safe candidate/record boundaries. Parallel tasks never
  observe a cross-task cancel because parallel tasks are never started under a
  budget.
- **Safe task retirement:** the parallel path retires no task early — all tasks
  join before concatenation. Early retirement exists only on the serial path,
  where "retirement" is ordinary loop exit. There is no speculative execution
  to cancel and no partial cross-task state to reconcile.
- **Ordered-prefix guarantees:** `OrderedPrefix` with any `threads` is serial
  and therefore prefix-exact by definition.

## Evidence anchors

- [`src/search.cpp`](../src/search.cpp): M5.2 eligibility gate (the four
  fallback conditions).
- [`src/cli.cpp`](../src/cli.cpp): quiet-to-FirstHit mapping.
- [`tests/test.cpp`](../tests/test.cpp): `pergrep_m54_max_match_cancel`.