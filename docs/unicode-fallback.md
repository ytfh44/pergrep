# Unicode fallback matrix (M6.4)

**Status:** Accepted (every uncertain query reaches the exact verifier)
**Scope:** M6.4 / GitHub issue #66

## Rule

Any Unicode uncertainty falls back to an unfiltered candidate path and the
unchanged ICU-aware exact verifier. Filters may only reject proven
non-matches (M6.1); when proof is unavailable, they reject nothing. The
fallback reason is observable in `SearchStats.qgram_fallback_reason`.

## Matrix

| Uncertainty | Fallback point | Recorded reason | Cost |
|---|---|---|---|
| Non-ASCII case-insensitive fixed literal | Chunk candidates unfiltered (all chunks) | `case-insensitive` | Full scan |
| Width-changing folds (Kelvin→k) | Same as above; verifier spans the wider text | `case-insensitive` | Full scan |
| Unicode property/class queries (`\pL`, `\w` unicode) | Regex verifier paths, never the folded hook | Path's own reason (never `case-insensitive-folded`) | Unfiltered chunks |
| Invalid byte sequences in text | Binary policy + byte-wise verifier (M6.1/M5.5) | Policy-dependent | Full scan |
| Truncated sequence in pattern | Outside oracle parity (M6.1 scope note); exact verifier, deterministic | N/A (no parity claim) | Full scan |
| Mixed normalization (precomposed vs decomposed) | No match by construction (no normalization, M6.1) | N/A | — |
| Scoped flags (`(?i:)`, `(?-i:)`, `(?u:)`) | Regex verifier (fixed-only hook never applies) | Non-folded | Unfiltered |
| Word/line scoping, files modes | Excluded from folded eligibility (M6.2) | `case-insensitive` | Full scan |
| Loaded snapshot (no folded groups) | Folded hook absent → fallback | `case-insensitive` | Full scan |
| Eligible ASCII control | Folded filter active | `case-insensitive-folded` | Pruned |

## ICU stability

Behavior is pinned, not versions: `pergrep_m64_unicode_fallback` asserts the
exact folds M6 relies on (ß→ß, Kelvin→k, é→é, Β→β, β→β, A→a). An ICU update
that changes any of these fails loudly instead of silently drifting results.
`u_foldCase` simple (not full) folding is the contract; compatibility
expansions (ß→ss, ſ→s) never apply.

## Cost accounting

Fallback cost is visible as `candidate_chunks == total chunks` (nothing
pruned) versus pruned counts on the eligible path. Estimates stay
conservative upper bounds (M6.2); M1 owns plan visibility.

## Evidence anchors

- [`src/search.cpp`](../src/search.cpp): folded hook gate, reason strings.
- [`tests/test.cpp`](../tests/test.cpp): `pergrep_m64_unicode_fallback`
  (called from `main`, defined above it — see M6.3 note on MSVC main size).
- [`docs/folded-filter.md`](folded-filter.md): eligibility; M6.1 oracle scope.