# Pattern metadata propagation in multi-pattern search (M7.4)

**Status:** Accepted
**Scope:** M7.4 / GitHub issue #74

## What

Shared scanning must not discard pattern-specific match data. M7.4 defines
how captures, named groups, replacement inputs, only-matching spans, JSON
record inputs, and per-pattern errors propagate through `find_multi`, and
proves each equals independent execution.

## Layering

- **M7 owns match data.** `find_multi` returns per-`source_id` `Match`
  vectors (spans + captures with names and match flags) identical to each
  pattern's own `find`. Every consumer downstream — replacement expansion
  (`interpolate_replacement`), only-matching output, JSON records — renders
  from those `Match` values, so identical inputs render identically. CLI
  maintainers own presentation compatibility (single-pattern rendering is
  already covered by `cli_compat`).
- **Fixed patterns strip captures on every path.** Serial `find` strips
  captures for fixed literals (documented hot path); the Aho-Corasick
  post-process strips them the same way. Replacement `$1` on a fixed pattern
  therefore expands empty under both — identical by construction. Only `$0`
  (whole-match spans) carries data, and spans are byte-identical.

## Metadata rules

- `has_captures` is true only for regex patterns whose program defines
  groups. Fixed literals never carry captures even when their bytes parse
  as group syntax (fixed: the literal `(a)` searches parentheses, it does
  not group). Fixed in M7.4 (was: regex parse of the literal text).
- `needs_replacement` marks callers that will render replacements; it is
  planning metadata only (a future shared path that drops captures must
  refuse entries with this flag — no such path exists yet).
- Errors keep pattern identity: `compile_multi_query` compiles a batch and
  throws `MultiPatternCompileError` (a `std::runtime_error` carrying
  `source_id` plus the underlying message) on the first failure, so one bad
  pattern among many reports exactly which one failed with the same message
  its own `Pattern::compile` would produce.
- Runtime failures (e.g. VM resource limits) propagate from the failing
  member's serial `find` with identical type and message; `find_multi`
  offers no partial results on throw, matching an independent loop's
  observable behavior.

## Non-goals

A result-merging presentation layer (no consumer yet), changing the AC
whole-IR gate for capture-carrying fixed patterns (nothing to gate: both
sides strip), per-pattern error *recovery* (fail-fast matches independent
execution).

## Evidence anchors

- [`include/pergrep/pergrep.hpp`](../include/pergrep/pergrep.hpp):
  `MultiPatternCompileError`, `compile_multi_query`.
- [`src/search.cpp`](../src/search.cpp): `has_captures` fix, batch compiler.
- [`tests/test.cpp`](../tests/test.cpp): `test_m74_pattern_metadata`.