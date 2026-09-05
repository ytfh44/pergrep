# Parallel output-mode transparency (M5.5)

**Status:** Accepted (library proof + downstream-by-construction argument)
**Scope:** M5.5 / GitHub issue #59

## Claim

Parallel execution (`SearchOptions.threads > 1`) is transparent to every CLI
and library output mode: for the same pattern, options, and index, the parallel
merge returns byte-identical `Match` vectors (file_id, start, end, captures) to
the serial path, so everything downstream — formatting, counting, exit status —
behaves identically.

## Mode-by-mode status

Proven at the library level by `pergrep_m55_semantics` (plus M5.2–M5.4 blocks):

- `invert_match`, files-with/without-match: per-file scope-in/scope-out is
  order-preserving under concatenation (M5.2); re-pinned here with binary files
  present.
- Binary handling (`include_binary` true/false, NUL/high bytes): verifier
  filtering is per-file; threads do not change the binary decision.
- Record separators (`'\n'` and NUL) and NUL content: separator handling is
  per-file verification; identical under threads.
- Case modes (sensitive/insensitive/smart), multiline, dotall: pattern options
  flow into each per-file sub-search unchanged.
- Captures (numbered and named): carried per-`Match` through concatenation;
  replacement inputs are therefore identical.
- Counts: `Match` vector sizes are equal, so every CLI count aggregation
  (`--count`, `--count-matches`, files-with summary counts) agrees.
- Statistics: requesting `SearchStats*` forces the serial fallback, so stats
  are computed once, on one path, regardless of `threads`. The deterministic
  counters (candidates, verified/touched bytes, matches, candidate files) are
  pinned equal across thread counts.
- Selector filtering: CLI glob/type selection compiles to an eligible file-ID
  scope; scoped parallel search equals scoped serial search (M5.2 subset case).

Downstream by construction (consume `find` results; never branch on `threads`):

- Context lines, JSON/vimgrep/pretty output, `--replace` rendering, sort/sortr,
  headings, colors: pure functions of the identical `Match` vector plus corpus
  content. No `threads` input reaches them.
- Exit status: derived from emptiness/counts of the identical result set
  (quiet → `FirstHit`, M5.4). CLI `-j/--threads` wiring is M5.7; once wired,
  these modes inherit transparency without further changes.

## Evidence anchors

- [`src/search.cpp`](../src/search.cpp): per-file sub-search copies the full
  `SearchOptions` (only `threads` reset, scope narrowed).
- [`tests/test.cpp`](../tests/test.cpp): `pergrep_m55_semantics`.