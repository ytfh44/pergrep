# MultiQueryIR and pattern identity (M7.1)

**Status:** Accepted (foundation; execution in M7.2+)
**Scope:** M7.1 / GitHub issue #71

## What

`MultiQueryIR` is the shared multi-pattern planner input: an ordered vector of
self-contained `MultiPatternEntry` values built by `make_multi_query_ir`.
Each entry owns its metadata (expression string, pattern options, scalar
search semantics, sorted/deduped eligible IDs, capture/replacement flags) —
no borrowed spans or callbacks — so the IR outlives the caller's objects.

## Identity rules

- `source_id` is the pattern's position in the user's list. It is identity,
  never reused: duplicate expressions keep distinct source IDs — and therefore
  distinct report rows, counts, and replacement behavior.
- The semantic key (`operator==`, `semantic_hash` FNV-1a) covers expression,
  all pattern-option fields, scalar search semantics, and eligible IDs.
  Cancellation hooks are execution-only and excluded (PlanKey rule).
- Duplicates share a semantic key (`entries[0] == entries[2]`,
  `hash[0] == hash[2]`) with different `source_id`s.

## Per-entry metadata

- Kind, case/word/line/multiline/dotall/unicode/crlf, overlap/invert/
  files-modes/binary/max-matches/separator/objective/threads, eligible scope.
- `has_captures`: pattern defines capture groups (from the parsed program).
- `needs_replacement`: caller will render replacements (CLI `--replace`).
- `threads`: carried per entry (M5 execution contract composes per pattern).

## Sharing report

`multi_query_sharing` groups source IDs whose scans may be shared; groups plus
singletons cover every entry exactly once, in source order. Current scaffold
rule: identical fixed literals share one scan; every regex stays independent.
`explain_multi_query_sharing` renders the human-readable plan
("patterns [0,2] share one fixed-literal scan for 'alpha'; pattern 1
independent (regex); ..."). M7.2/M7.3 refine the rule; the report shape is
stable.

## Non-goals

No execution change (the CLI still runs independent searches); no automaton;
no cross-pattern result merging (M7.5); no scope pushdown (M7.6).

## Evidence anchors

- [`include/pergrep/pergrep.hpp`](../include/pergrep/pergrep.hpp): types.
- [`src/search.cpp`](../src/search.cpp): builder, sharing, explain.
- [`tests/test.cpp`](../tests/test.cpp): `test_m71_multi_query_ir`.