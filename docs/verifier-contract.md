# Raw-byte exact verifier contract (M6.1)

**Status:** Accepted (authoritative; all engine work preserves it)
**Scope:** M6.1 / GitHub issue #63

## Rule

The exact verifier is authoritative. Approximate index structures (chunk
filters, Bloom rows, positional blocks, q-gram plans) may only **reject
proven non-matches**; any candidate that cannot be proven absent is verified
against raw source bytes, so filters can cause extra work but never a false
negative. Every engine variant must compare equal to the raw-byte reference
(`detail::regex_find_all` via `full_reference`) on the M0 oracle cases.

## Semantics

- **Invalid UTF-8:** malformed bytes (lone continuations, truncated sequences,
  overlongs, surrogates) are data, never errors. Matching proceeds byte-wise
  over them; a pattern byte matches a text byte. No replacement, no skipping.
- **Byte offsets:** `Match.start/end` and every `Capture` span are byte offsets
  into the source file, never rune indices. Multibyte content shifts offsets
  but never redefines them.
- **Code-point traversal:** forward progress advances by whole code points
  (rune-at), so overlapping matches step over multibyte characters without
  splitting them; invalid bytes advance by one byte.
- **ICU simple folding:** case-insensitive comparison folds each code point
  with `u_foldCase` (`U_FOLD_CASE_DEFAULT`) — e.g. `K`/Kelvin-sign/U+212A all
  fold together. No normalization is applied: canonically equivalent but
  differently encoded text (precomposed vs decomposed) does NOT match. Width or
  compatibility mappings are never introduced implicitly.
- **Capture spans:** group captures carry absolute byte spans, including across
  multibyte characters and chunk-region boundaries; unmatched groups report
  `matched=false`. Fixed-string matches carry no captures (allocation-free hot
  path); the raw oracle retains group 0, so conformance pins indexed-side
  emptiness plus span equality for fixed patterns and full capture equality
  for regex patterns.
- **Record separators:** `'\n'` default, NUL on opt-in. Separation is a
  per-record byte scan; separators inside multibyte sequences cannot occur in
  valid UTF-8 and invalid bytes never create records.
- **Filter-reject rule:** any filter row consulted must be a necessary
  condition for a match. Conservative hash-bucket bounds are never used to
  reject candidates (see planner notes in `src/search.cpp`).

## Oracle scope

The raw reference oracle (`detail::regex_find_all`) expresses patterns as code
points. Text may be arbitrary bytes (invalid UTF-8 is data), but a *pattern*
containing a truncated UTF-8 sequence is outside oracle parity: the indexed
path matches it byte-wise (pinned by an explicit span assertion), while the
oracle matches nothing. Prefer valid-UTF-8 patterns for oracle-parity cases.

## Engine variants (all bound by this contract)

`FixedRareByte`, `FixedPositional`, `RegexChunk`, `RegexBruteForce`
(`VerifierKind`), plus the extended PCRE2-compat VM for lookaround/backref
patterns. Selection is a performance decision; observable results are
identical. Any doubt falls back to the existing NFA/VM path, never to a
weaker semantic.

## Evidence anchors

- [`src/regex.cpp`](../src/regex.cpp): `fold`, `cp_eq`, rune advancement.
- [`src/search.cpp`](../src/search.cpp): verifier selection, filter use.
- [`tests/test.cpp`](../tests/test.cpp): `pergrep_m61_verifier_contract`.