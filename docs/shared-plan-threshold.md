# Shared-plan threshold and fallback (M7.7)

**Status:** Accepted
**Scope:** M7.7 / GitHub issue #77

## What

Admission to shared scanning is a deterministic predicate over the IR and
index sizes — never timing — so the same inputs decide alike. The decision
and the fallback reason are exposed via `SearchStats`: `physical_operator`
names the path taken, `qgram_fallback_reason` carries the first
disqualifier (`"none"` when a shared path runs).

## Gates

- **Pattern count (AC).** Fewer than 4 patterns falls back: measured
  crossover is below 2, the floor at 4 keeps wide margin. More than 256
  patterns falls back (setup and fan-in stay bounded for high-cardinality
  pattern files).
- **Literal budget (AC).** Total shared bytes capped at 64 KiB.
- **Memory budget (AC).** Automaton nodes capped at 16K; nodes are ~1 KiB
  (256-way transitions), so transient worst case is ~17 MB per `find_multi`,
  freed after. Build refusal falls through to grouped sharing, then to
  independent — never an error.
- **Entry eligibility (AC).** Fixed, case-sensitive, unanchored,
  exhaustive, single-threaded, `\n`-separated, non-empty literals with no
  mode flags; anything else disqualifies the whole IR from AC (grouped
  sharing may still admit its regex members).
- **Grouped sharing.** Needs a literal mandatory in 2+ members whose file
  set prunes (self-tuning: groups that prune nothing dissolve, so setup is
  never paid without benefit). No count floor and no corpus-size floor:
  calibration shows wins down to single-file corpora.
- **Reason priority.** A formed group resets the reason to `"none"`. On
  full fallback, IRs with regex candidates report the grouped reason (no
  commonality / no pruning); otherwise the AC disqualifier is reported.

## Calibration (M0-scale probe, 50 files x ~2 KB, best-of runs)

| case | independent | shared | path |
|---|---|---|---|
| fixed n=2 | 1.62 ms | 1.63 ms (parity) | IndependentFallback |
| fixed n=3 | 2.85 ms | 2.81 ms (parity) | IndependentFallback |
| fixed n=4 | 5.55 ms | 0.38 ms (14x) | AhoCorasickShared |
| fixed n=8 | 8.30 ms | 0.39 ms (21x) | AhoCorasickShared |
| fixed n=16 | 12.32 ms | 0.56 ms (22x) | AhoCorasickShared |
| fixed n=200 | 459.63 ms | 1.60 ms (288x) | AhoCorasickShared |
| regex shared pair | 40.59 ms | 1.82 ms (22x) | SharedPrefilterGroups |
| tiny corpus n=4 | 3.85 ms | 0.02 ms (180x) | AhoCorasickShared |

Fallback is parity by construction (the same serial code path); sharing
wins everywhere it engages, including tiny corpora and near the
high-cardinality rails. M9 owns release thresholds and rollback; this
policy is the mechanism it tunes.

## Non-goals

Timing-adaptive dispatch (nondeterministic, untestable), cross-IR plan
caching, changing the AC/grouped mechanisms (M7.2/M7.3/M7.6).

## Evidence anchors

- [`src/search.cpp`](../src/search.cpp): admission predicate + reasons.
- [`tests/test.cpp`](../tests/test.cpp): `test_m77_shared_threshold`.