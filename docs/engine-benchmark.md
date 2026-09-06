# Engine portfolio benchmark (M6.8)

**Status:** Accepted (decisions from evidence; defaults switch only here)
**Scope:** M6.8 / GitHub issue #70

## What

The `engine.portfolio.mixed` bench scenario exercises the engine portfolio on
the small generated repository (2 iterations): extended lookahead/lookbehind/
backref, capture vs no-capture twins, long alternation, and a repeat-cap
resource case — each with compile time, verification time, operator,
fallback reason, and match counts. The bench correctness gate compares every
query against the reference (`correctness=pass` required).

## Measured snapshot (Windows clang-cl, small corpus, 2 iterations)

| Query | Operator | Compile | Verify CPU | Matches |
|---|---|---|---|---|
| extended-lookahead `er(?=ror)` | RegexBruteForce (lookaround) | 0.03ms | 837ms | 3648 |
| extended-lookbehind `(?<=timeout=)connection` | RegexBruteForce (lookaround) | 0.02ms | 9677ms | 3 |
| extended-backref `([a-z])\1` | RegexBruteForce (backreference) | 0.01ms | 1482ms | 28848 |
| capture `connection_([a-z_]+)` | RegexBruteForce | 0.04ms | 71ms | 3144 |
| nocapture twin `connection_[a-z_]+` | RegexBruteForce | 0.02ms | 63ms | 3144 |
| long alternation (10 branches) | RegexChunk | 0.10ms | 1105ms | 37521 |
| repeat-cap `a{1,100000}` | RegexBruteForce | 58.8ms | 4274ms | 84219 |

Absolute numbers are machine-specific; the SHAPE is the verdict (see below).
Timings come from `verifier_cpu_ms` / new `compile_ms` per-query fields.

## Per-path verdicts (target workload, oracle, bound, rollback, decision)

- **Fixed strings** (rare/common, short/long): rare-byte/positional filters.
  Target: everything. Oracle: exact. Bound: chunk/positional rows.
  Rollback: gate removal. Decision: KEEP (default, proven M0–M6).
- **Bounded regex** (`ID_[0-9]{4,6}` family): `RegexBoundedRegion` one-pass.
  Target: finite bounded patterns. Oracle: exact incl. captures. Bound: 1MiB
  width, mandatory literals. Rollback: eligibility false. Decision: KEEP (M6.5).
- **General regex** (alternation, classes): `RegexChunk` pruning + NFA.
  Target: regular patterns. Oracle: exact. Bound: Bloom rows, 50k states.
  Rollback: brute force. Decision: KEEP. (Long alternation measures 10
  branches pruning correctly here.)
- **Extended VM** (lookaround/backref): `RegexBruteForce` + exact VM.
  Target: lookaround/backref only. Oracle: exact. Bounds: 8192 window,
  10000 repeat/10000 depth/50000 states, all throwing or counted (M6.7).
  Rollback: N/A (only exact path). Decision: KEEP as the sole extended path;
  no DFA promotion (M6.6 verdict stands). Lookbehind dominates cost (9.7s
  here) — expected: per-position window scans.
- **Case-insensitive ASCII**: folded Bloom + ICU verify (M6.2). Target: ASCII
  `-i`. Oracle: exact. Bound: twin Bloom bytes. Rollback: unfiltered.
  Decision: KEEP.
- **Captures**: group capture costs ~13% vs the no-capture twin on the same
  spans (71ms vs 63ms); spans identical (3144 = 3144). Decision: KEEP always-
  correct captures; no capture-elision optimization (M6.6: TDFA tag complexity
  not justified).
- **Repeat-cap patterns**: compile cost is real (59ms for a 100k bound —
  counted, not hidden); verification stays bounded. Decision: KEEP caps +
  compile-time reporting (new `compile_ms` per-query field).

## No default switches in M6.8

Every path keeps its current default: each switch above requires a target
workload regression in this matrix plus M9 rollout approval. The matrix
itself is the switch harness — add the workload, measure, then decide.

## Evidence anchors

- [`bench/workload_matrix.hpp`](../bench/workload_matrix.hpp): portfolio
  queries + `engine.portfolio.mixed` scenario.
- [`bench/bench.cpp`](../bench/bench.cpp): `compile_ms` timing + reporting.
- [`tests/test.cpp`](../tests/test.cpp): `test_m68_engine_matrix` (below).
