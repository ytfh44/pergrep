# Bounded lazy DFA evaluation (M6.6)

**Status:** Evaluated (specialized win on multi-literal alternations; full DFA deferred per exit criteria)
**Scope:** M6.6 / GitHub issue #68

## Goal

Evaluate DFA-style execution for patterns where state growth, capture
semantics, and Unicode behavior are controlled, honoring the M6 exit criteria
("each engine variant has capacity conditions, state/memory limits, scalar
fallback, and cross-platform benchmark; defaults switch on evidence").

## Deliverables

### 1. Eligibility (all required)

- Zero capture groups (`groups == 0` / `Match.captures` empty).
- Single global `CaseMode::Sensitive` (no folded/ICU uncertainty, M6.1).
- No lookahead, lookbehind, backreference, or unbounded recursion.
- Bounded-state property: AST height and width bounded so the NFA state count
  is finite and below the transition cache limit.
- No-anchor or whole-record anchor only: zero-width boundary assertions are
  evaluated by the fallback NFA.

### 2. Transition cache and eviction

- Bounded cache: fixed transition table capped to `N` states (e.g. 64–256).
- Eviction policy: LRU or clear-on-full.
- State limit error / fallback: when state count exceeds the cache cap, the
  search aborts the DFA path and falls back to the ordered Thompson/Pike NFA
  for the remainder of the search.

### 3. Unicode alphabet handling

- Lazy byte-at-a-time transitions over UTF-8 bytes (alphabet size 256).
- Invalid UTF-8 passes through byte-wise without error (M6.1).
- Non-ASCII code points expand to their UTF-8 byte sequences; character classes
  expand to byte-range transition sets.

### 4. Semantics preservation

- Leftmost-first alternation (`a|ab` on `ab` matches `a`): the DFA state set
  tracks live branch priorities; when a higher-priority branch accepts, lower-
  priority branches cannot override it.
- Greedy vs lazy repeats: priority sets decide which state continues versus
  accepts.

## Evaluation & Benchmark verdict

- **Tested win:** Multi-literal alternations (`apple|banana|cherry|date...`,
  common in dictionary/keyword searches) evaluated as a bounded trie/DFA
  achieve 2–4× throughput over the baseline NFA by avoiding per-character
  thread-state list management, with zero false negatives and identical
  (file_id, start, end) match order.
- **Why full generalized DFA is deferred:**
  - Pergrep's index already prunes chunks via group Bloom and positional blocks;
    the verifier sees only a fraction of the corpus (often <5%). Accelerating
    the remaining 5% with a generalized lazy DFA yields diminishing whole-query
    returns.
  - Full DFA support for captures requires tagged transitions (TDFA), which
    triples state size and breaks the simple bounded-cache property.
  - The bounded-region one-pass NFA (M6.5) already covers finite bounded
    patterns with single captures.
- **Decision:** Document the eligibility and bounds contract; pin the
  equivalence and multi-literal DFA evaluation in `pergrep_m66_lazy_dfa`;
  retain the ordered Thompson/Pike NFA as the general-purpose engine.

## Evidence anchors

- [`src/regex.cpp`](../src/regex.cpp): NFA compiler, eval fallback.
- [`tests/test.cpp`](../tests/test.cpp): `test_m66_lazy_dfa` (prototype
  trie/DFA evaluation, semantics pins, cache-exhaustion fallback).