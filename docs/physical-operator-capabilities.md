# Physical operator capabilities (M1.4)

This is an implementation inventory for issue #23. It describes the operators that
exist before a new scheduler is introduced. It is intentionally conservative: a
capability is marked supported only when the current code reaches it and performs
an exact verification with the stated semantics. `FixedChunk` is an internal
shape, not a public `VerifierKind`; its `SearchStats::verifier` value is
`FixedRareByte`.

## Estimates, guarded dispatch, and fallback

Searcher::find computes a complete PlanKey and calls estimateCost(PlanKey,
IndexData). M1.7 uses that estimate for physical dispatch only when the explicit
M1.7 guard below proves that all candidate operators have identical semantics.
Every other query retains the established hard-coded order:

1. `invert_match` is a record-enumeration wrapper around fixed or regex exact
   checks.
2. Fixed patterns whose byte length is greater than `IndexOptions::chunk_overlap`
   use a whole-file rare-byte scan over the candidate-file union.
3. Fixed patterns with case-insensitive, `word`, or `line` options use
   chunk-level candidate pruning and rare-byte verification.
4. Other fixed patterns of length at most 64 bytes use positional block
   filtering.
5. Remaining fixed patterns use chunk-level candidate pruning and rare-byte
   verification.
6. A regex that is a pure, sensitive, non-extended literal is converted to the
   fixed path when `multiline` is set or the literal does not contain the
   requested `SearchOptions::record_separator` (`src/search.cpp:1891-1897`).
   Capture-bearing literal-shaped regexes are excluded from this conversion so
   their captures remain owned by the general regex path. Other regexes use
   branch-mandatory/mandatory chunk pruning when available, otherwise the
   unpruned candidate set.
Regexes still use branch-mandatory/mandatory pruning when available, otherwise
the unpruned candidate set. The guarded fixed dispatcher never applies to regex
conversion, capture-bearing patterns, or any multi-pattern path.
## M1.7 guarded cost-based fixed dispatch

M1.7 enables physical selection only for a deliberately narrow, provably equivalent
contract. Searcher::find builds the complete PlanKey; the guard requires all of:

- a direct fixed pattern with 4--64 bytes, no cross-chunk possibility
  (4 <= q.size() <= 64 and q.size() <= chunk_overlap);
- sensitive matching, with word, line, multiline, dotall, crlf disabled,
  Unicode enabled, and the default fixed engine;
- newline records, unlimited non-overlapping positive matches, no file-polarity
  wrapper, no inversion, no eligible-file selector, and binary inclusion disabled;
- a PlanKey whose IndexOptions exactly match the live index, with planner statistics
  ready, non-empty positional data, a positive positional block size/q-gram budget,
  and no binary files in the corpus.

Any failed predicate is a conservative fallback to the pre-M1.7 length/option
branches. Estimates never decide candidate membership: q-gram and Bloom filters
remain supersets, and exact verification remains owned by the existing operators.
For a guarded query, calibrated M1.6 candidate-work units rank positional-block,
chunk-level rare-byte, and whole-file rare-byte backends. Ties retain the existing
positional preference. Whole-file selection is safe in this guard because the
literal is short and no boundary or scope semantics are active; literals that can
cross a chunk boundary are never admitted. The calibration is intentionally static
and tied to the M1.6 shadow units (probe/enumeration plus verification bytes); if
planner statistics or capability identity is unavailable, selection is disabled.

SearchStats::physical_operator reports the concrete backend
(FixedPositional, FixedChunk, FixedRareByteWholeFile, or a wrapper label),
while guarded_dispatch_used distinguishes a guarded choice from fallback.
verifier remains the stable public verifier kind (FixedRareByte covers both
rare-byte shapes), and verifier_fallback is true for fixed queries outside the
guarded contract. Bench query metrics print both fields. Rollback is therefore a
configuration/dispatch guard change: disable the guard or fall back to the existing
branch without changing match order, captures, offsets, overlap/max-count,
record-separator, binary, or selector semantics.

### Wrapper semantics that affect every row

- `Searcher::find` returns matches. Its `files_with_matches` and
  `files_without_match` fields do not change that match list; `Searcher::files`
  calls `find` with an unlimited match count and then applies file polarity.
  `files_without_match` wins if both file-polarity flags are set.
- `invert_match` is handled before physical fixed/regex dispatch. It scans
  logical records, touches each eligible non-binary file, and emits records
  for which the exact fixed or regex check is false. Thus its
  `SearchStats::verifier` is still the cost-model annotation, not proof that
  the underlying positional/chunk operator ran.
- `include_binary` is checked before exact verification, but candidate counters
  are recorded at filter time. A binary file can therefore contribute to
  `candidate_chunks`/`candidate_files` and then contribute no matches or
  touched bytes when binary inclusion is disabled.
- For a direct `PatternKind::Fixed` pattern, regex-only syntax and the
  `Engine` value are not parsed; fixed matching is literal byte/rune matching
  with the explicitly handled `case_mode`, `word`, `line`, `unicode`, and
  `crlf` options.


## Capability table

Legend: **yes** means implemented and exercised by this operator; **no** means
this operator must not be used for the combination; **wrapper** means the
behavior is supplied outside the physical operator; **conditional** names the
predicate that must hold; **not a separate operator** means the behavior is an
exact backend used by a row above.

| Physical operator | Triggering predicate in current code | Candidate granularity and exact verifier | Boundary/context support | Captures, ordering, overlap, max-count, invert, and files | `SearchStats` accounting | Setup cost, fallback, and unsupported combinations |
| --- | --- | --- | --- | --- | --- | --- |
| **FixedPositional** | `Pattern::is_fixed()`, non-empty `q.size() <= 64`, `q.size() <= chunk_overlap` (the longer-literal branch is tested first), case-sensitive, and neither `word` nor `line` (`search.cpp:1388-1451`, `1515-1549`). Empty fixed literals currently enter this branch but `fixed_candidate_blocks` emits only block 0, so they are explicitly **unsupported** here and remain covered by the zero-width regression contract rather than this capability row. | `chunk_candidates` first emits candidate chunks from q-gram groups; `fixed_candidate_blocks` intersects positional Bloom rows and emits `(chunk, block)` pairs. Each block is verified by an exact rare-byte anchor scan over the block core plus up to 64 bytes of lookahead (`search.cpp:380-435`, `1515-1549`). | Raw byte literal matching, including NUL bytes: **yes**. `record_separator`, CRLF, `word`, and `line` context: **no** (block context is unsafe, so dispatch excludes `word`/`line`). `multiline`, `dotall`, and `unicode` do not add boundary semantics to a fixed raw literal. | Captures: **no** (`Match::captures` is empty for fixed matches). Emission follows file/chunk/block traversal; no final global sort. `overlapping` and `max_matches`: **yes**. `invert_match`: **not this operator**; the invert wrapper bypasses block execution. `files_with_matches` / `files_without_match`: **wrapper** via `Searcher::files`. `include_binary` and eligible-file scope are enforced while candidates are verified. | `candidate_chunks`: q-gram candidate chunks; `candidate_blocks`: positional pairs emitted; `index_probe_bytes`/`index_probe_operations`: positional Bloom rows; `physically_touched_bytes` and legacy `verified_bytes`: block slices (including lookahead); `logical_unique_bytes`: union of touched slices; `candidate_files`: files seen in candidate chunks; `matches`: output count; `verifier_cpu_ns`: timed candidate generation + verification. `verifier` is the cost-model label `FixedPositional`; `verifier_fallback` is normally false. | Builds/allocates q-gram row intersections and per-candidate block masks; `planned_hashes` selects up to adaptive `k` rare q-grams (bounded at 8). q-gram queries shorter than 4 bytes or all-common q-grams conservatively widen to all chunks/blocks. Unsupported: empty literals, case-insensitive, `word`, `line`, and literals crossing `chunk_overlap`; those use a different path or remain a known unsupported edge. |
| **FixedChunk (chunk-level fixed path)** | Fixed literal with `q.size() <= chunk_overlap` that is not eligible for positional blocks: normally `64 < q.size() <= chunk_overlap`; the same chunk-level anchor loop is used for case-insensitive, `word`, or `line` fixed queries (`search.cpp:1451-1507`, `1550-1586`). | Candidate granularity is chunk. `chunk_candidates` uses q-gram group bitsets when the literal is 4..`chunk_overlap` bytes; otherwise all eligible chunks pass. Exact verifier scans each chunk's extended slice with a rare-byte anchor and starts only in its core, deduplicating overlap with a per-file next position (`search.cpp:174-197`, `1451-1507`, `1556-1585`). | Raw fixed bytes and NUL: **yes**. For `word` and `line`, exact verification reads the full loaded file for adjacent context; ASCII or ICU word predicates follow `PatternOptions::unicode`. `line` uses `SearchOptions::record_separator`, including the implemented CRLF exception when separator is LF and `crlf` is set. A non-boundary fixed literal is not record-split and can contain/bridge separators when the overlap permits it. | Captures: **no**. Emission is source traversal order (file/chunk core order), with overlap suppression for non-overlapping searches. `overlapping`, `max_matches`: **yes**. `invert_match`: **wrapper**, not chunk verification. Files selectors: **wrapper**; binary policy and eligible IDs apply to candidate/verification loops. | `candidate_chunks` and `candidate_files` come from q-gram pruning; `candidate_blocks` remains 0; q-gram group reads increment `index_probe_bytes`/`index_probe_operations`; `physically_touched_bytes`/`verified_bytes` count each chunk extended slice, while `logical_unique_bytes` unions overlaps; `matches` and `verifier_cpu_ns` are populated. `verifier` is `FixedRareByte` (there is no `FixedChunk` enum value). | Group bitset probing plus chunk-slice anchor scanning. For literals shorter than 4 bytes, q-gram pruning is disabled and all eligible chunks are candidates. If `q.size() > chunk_overlap`, this is unsafe for cross-chunk matches and execution falls through to the whole-file `FixedRareByte` variant. Positional blocks are not used with case folding or boundary context. |
| **FixedRareByte (whole-file long-literal fallback)** | Fixed literal with `q.size() > IndexOptions::chunk_overlap` (`search.cpp:1392-1440`). | `chunk_candidates` returns all chunks for a literal longer than overlap; their file IDs form a conservative union. Exact verifier scans each selected file's entire loaded byte string with the rarest-byte anchor, so a match crossing any chunk boundary is retained. | Raw literal and NUL: **yes**. `word` and `line`: **yes**, checked against full-file neighbors; `line` honors the requested separator and implemented CRLF exception. `record_separator` affects line/invert behavior, not a plain raw fixed search. `multiline` is not a separate fixed check; raw bytes may span separators. ICU case-folding is used for case-insensitive matching; fixed `unicode` controls the word predicate rather than disabling that fold. | Captures: **no**. File order and increasing anchor positions; no final sort. `overlapping` and `max_matches`: **yes**. `invert_match`: **wrapper**. Files selection is **wrapper** (`Searcher::files` runs an unlimited find first, then selects hit/miss files); binary inclusion and eligible IDs apply. | `candidate_chunks`/`candidate_files`: all chunks/files in the candidate union (subject to eligible IDs; binary files can still be counted as filter candidates when later skipped); `candidate_blocks`: 0; index probes normally 0 for this fallback; `physically_touched_bytes`/`verified_bytes`: whole selected file; `logical_unique_bytes`: whole-file union; `matches`, `verifier_cpu_ns`: populated. `verifier` is `FixedRareByte`; `verifier_fallback` is true because the fixed expression exceeds overlap. | The safety fallback intentionally trades pruning for a whole-file touch. It is the only current fixed path that guarantees long literals crossing chunk boundaries. It does not use positional blocks. |
| **RegexChunk + Thompson NFA** | Regex is not converted to pure fixed, global `case_mode` is not `Insensitive`, and `QueryIR::branch_mandatory` or `mandatory` is non-empty; default/non-extended regex compilation creates an NFA (`regex.cpp:1012-1019`). With global insensitive mode, the current search passes an empty literal to `chunk_candidates`, so candidate pruning is unavailable and all chunks may be scanned. | Candidate granularity is chunk. Top-level alternation branches use a union of each branch's longest mandatory literal; otherwise the longest global mandatory literal drives `chunk_candidates` (`search.cpp:1597-1618`). Exact verification then runs `regex_find_all` on the complete candidate file, split into records unless `multiline` is enabled. The exact backend is `nfa_search`, not the q-gram filter (`regex.cpp:545-605`, `1022-1024`). | `record_separator`: **yes** for record splitting and NFA assertions. `multiline`, `dotall`, `line`, `word`, CRLF: **yes** through NFA assertions and parser wrappers; `line`/`word` are represented as begin/end or half-word assertions. Unicode rune decoding, ICU case folding, Unicode properties/classes, and NUL code points: **yes**; `unicode=false` selects ASCII class/word behavior where the parser uses it. Invalid UTF-8 falls back to single-byte handling. | Captures, including named captures: **yes** via `SaveStart`/`SaveEnd`. `regex_find_all` emits left-to-right matches; non-overlap advances to match end, overlap advances one decoded rune (or one byte on invalid input); zero-width matches always make progress. `max_matches`: **yes** (remaining global count passed per file). `invert_match`: **wrapper**; it calls one exact `regex_search` per logical record. Files selectors: **wrapper** via `Searcher::files`; binary and eligible scope apply. | `candidate_chunks`, `candidate_files`, q-gram `index_probe_*`: filter-stage evidence; `candidate_blocks`: 0. Exact regex touches the whole selected file once (`physically_touched_bytes`/`verified_bytes`), and `logical_unique_bytes` records the union. `matches` and `verifier_cpu_ns` are populated. `verifier` is `RegexChunk`; fallback is normally false. | Parse/compile builds QueryIR and an NFA (compiler limit: 1,000,000 instructions). Prefixes can let `nfa_search` jump to the next possible offset, but this is an exact-execution optimization, not a separate candidate operator. If a branch has no mandatory literal, branch pruning is disabled; nested alternation is not split into branch candidates. Mandatory literals are only exact, non-empty, sensitive necessary conditions; optional/negative constructs contribute no safe literal. |
| **RegexChunk + extended VM** | Same **case-sensitive** positive-literal candidate predicate as above, but the parsed regex contains backreferences or look-around and was compiled with a non-`Default` engine (`regex.cpp:277-281`, `1012-1019`). Under `case_mode=Insensitive`, the current path does not extract case-folded mandatory candidates and may scan all chunks. | Chunk candidates come only from conservative positive mandatory literals. Exact verification is whole-file/record `regex_find_all` backed by `eval`/the extended VM (`regex.cpp:619-705`, `1022-1024`). | The VM supports record separator, multiline, dotall, line/word, CRLF, Unicode/ASCII mode, NUL, look-ahead, look-behind, backreferences, greediness, and captures as implemented by the AST evaluator. Look-behind searches only the preceding 8192 bytes; matches requiring more context are **not guaranteed** and must be treated as unsupported. | Captures and named captures: **yes** when the extended VM reaches exact verification. Ordering is the VM's first returned state at the leftmost start, with `regex_find_all`'s left-to-right/overlap/max rules. `invert_match` and files are wrappers with the same limitations as the NFA row. | Same filter and whole-file exact-touch counters as RegexChunk + NFA; `candidate_blocks` is 0. The public `verifier` remains `RegexChunk`; `verifier_cpu_ns` includes VM evaluation. | Extended constructs are rejected when `Engine::Default` is requested; `Pcre2Compat`/`Auto` permit this repository's own extended VM (not an external PCRE2 execution path). Look-behind is bounded to 8192 bytes and silently limits the search window; repeats are explored only up to 10,000 iterations and can silently fail to find matches requiring more; intermediate VM state to 50,000 and recursion depth to 10,000 throw named errors. These bounded cases are not full-semantic capabilities and must not be advertised as such. If no safe mandatory literal exists, use RegexBruteForce + VM instead. |
| **RegexBruteForce + Thompson NFA** | Regex is not pure-fixed and both `branch_mandatory` and `mandatory` are empty; the candidate vector is therefore all chunks (`search.cpp:1611-1623`, `regex.cpp:1022-1024`). This label also describes the no-pruning estimate. | Candidate granularity is all chunks, collapsed to all candidate files. Exact verification is per-file/record or whole-file `regex_find_all` using the regular Thompson NFA. NFA prefix jumps may still skip impossible start offsets. | Same exact NFA support as the RegexChunk + NFA row: record separator, multiline, dotall, word/line, CRLF, Unicode/ASCII classes, NUL, and invalid-UTF-8 byte fallback are supported where the corresponding options/AST nodes are used. | Same NFA capture, source-order, overlap, zero-width, max-count, invert, files, binary, and eligible-scope behavior as RegexChunk + NFA. | `candidate_chunks` is all eligible chunks; `candidate_blocks`: 0; index probes generally 0 because the empty literal path returns all chunks; whole-file exact-touch and logical-union counters are populated; `candidate_files`, `matches`, and `verifier_cpu_ns` are populated. `verifier` is `RegexBruteForce`; `verifier_fallback` is true. | No q-gram setup; parse/NFA setup still occurs. This is a conservative fallback, not evidence that a regex has no useful exact prefix. No mandatory literal means no safe chunk filter under current QueryIR rules. |
| **RegexBruteForce + extended VM** | Extended regex with no safe positive mandatory literal, or the planning-only `PlanKey` compile-error fallback (`search.cpp:781-786`, `815-818`). A real `Pattern` that fails compilation is not searchable; the catch is only in cost estimation. | All chunks/files are candidates; exact per-file/record or multiline whole-file verification uses the extended VM. | Same VM support and resource bounds as RegexChunk + extended VM. | Same VM capture/order/overlap/max-count and wrapper semantics as the RegexChunk + extended VM row. | All-chunk filter counters, whole-file exact-touch counters, `matches`, and `verifier_cpu_ns`; `verifier` is `RegexBruteForce`; fallback is true. | No index pruning. Default-engine extended syntax throws during `Pattern::compile`; `estimateCost(PlanKey, ...)` returning a brute-force estimate after a caught compile error must not be read as a runtime fallback plan. VM resource-limit errors throw rather than switching operators. |

## Exact backend details (not separate public verifier kinds)

### Regular regex / Thompson NFA

`NfaCompiler` lowers regular AST nodes (`Literal`, `Dot`, `Class`, anchors,
word assertions, groups, concatenation, alternation, and repetition) into
Thompson-style instructions. It preserves captures through save instructions
and uses ICU rune/case/class helpers. Backreferences, look-ahead, and
look-behind are explicitly rejected by the NFA compiler; those constructs set
`RegexProgram::extended` and require the VM engine. The NFA's `prefixes` metadata
can accelerate start-position discovery, but remains an exact verifier detail.


### Extended VM limitations

The `eval` VM handles the extended AST, including captures, look-around,
backreferences, bounded/unbounded repetition, and greediness. Its explicit
resource behavior is mixed: look-behind searches only a preceding 8192-byte
window, and repeat exploration is capped at 10,000 iterations without a
dedicated error; these cases can miss matches requiring more context or
iterations. Recursion depth above 10,000 and intermediate state above 50,000
throw named runtime errors. These limits are unsupported combinations for a
full-semantic capability claim, not alternative dispatch predicates.

## SearchStats and M0.7 evidence

The public C++ counters have the following stable meanings (`pergrep.hpp:180-217`):

- `candidate_chunks`, `candidate_blocks`, and `candidate_files` describe filter
  output, not exact-match output. `candidate_blocks` is nonzero only when the
  positional path emits block pairs.
- `verified_bytes` is the legacy alias of
  `physically_touched_bytes`. `logical_unique_bytes` unions touched source
  ranges; physically touched overlap is counted per verifier slice.
- `index_probe_bytes` and `index_probe_operations` count bytes/rows read from
  in-memory candidate indexes.
- `matches` is the final `find` output count. `verifier_cpu_ns` is process CPU
  time measured around candidate generation and exact verification.
- physical_operator and guarded_dispatch_used expose the concrete backend and
  whether M1.7 selection ran. verifier remains the stable enum label
  (FixedRareByte covers chunk and whole-file anchor scans).
- verifier_fallback is true for fixed searches outside the explicit M1.7 guard,
  invert wrappers, and RegexBruteForce. It is false only for a guarded fixed
  choice; guarded_dispatch_used is the direct choice/fallback indicator.
- `plan_regret` is populated from M0.7's chosen-plan record. In `find`, the
  chosen candidate is observed and alternatives generated by
  `estimate_all_candidate_plans` are not counterfactually executed, so this
  path does not measure alternate physical runtimes.

M0.7 release evidence remains separate from operator capability: correctness
parity must cover match count, file IDs, byte offsets, captures, overlap, max,
CRLF, and NUL records; performance evidence includes search latency/throughput,
`verified_kb`, candidate chunks/blocks, and the warm/cold lifecycle split. The
release thresholds, regret definitions, fallback caps, and rollback triggers
are owned by [`docs/plan-regret-gates.md`](plan-regret-gates.md), while the
versioned query profiles and metric names are in
[`docs/workload-matrix.md`](workload-matrix.md). Counters alone do not prove a
scheduler selected the cheapest operator.

## M1.5 planner statistics

Each indexed four-byte window contributes to two intentionally separate
statistics families:

* `IndexData::qgram_freq[hash & 65535]` is the legacy **hash-bucket
  occurrence** counter. It counts windows (saturating at `UINT32_MAX`) and is
  used only by the conservative filter compatibility surface.
* `IndexData::exact_qgrams` is keyed by the raw four bytes, not the hash.
  `occurrence_frequency` counts every exact window; `chunk_frequency` counts
  distinct chunks whose extended view contains the q-gram; and
  `document_frequency` counts distinct indexed files containing it. These are
  counts, not probabilities. Their ID vectors make the same counts available
  for an eligible-file selector without assuming selected files have the
  global distribution.

For collision safety, `hash_chunk_freq`/`hash_chunk_ids` count distinct chunks
per legacy hash bucket. A planner signal is `max(exact chunk frequency, hash
bucket chunk frequency)`, divided by the eligible chunk count. This is an
upper bound on the chunks that a hash-based filter can emit: collisions can
widen work, but cannot make the planner reject a true match. A q-gram shorter
than four bytes has no q-gram selectivity signal and predicts all eligible
chunks. Estimated verified bytes are candidate chunks times the configured
chunk size (or all candidate blocks times the same bounded unit); this is a
work estimate, not a claim about source-byte occurrences or I/O.
For bounded resource use, exact table construction is enabled up to the
deterministic 128 MiB corpus cap and one million distinct raw q-grams. Above
either bound, planner tables are cleared and marked unavailable; planning
returns selectivity 1.0 (all eligible chunks), while legacy hash filters and
exact verification remain unchanged. This is intentionally uncalibrated
fallback behavior, not a reinterpretation of occurrence counts.

The v5/v6 on-disk layout is unchanged. Those versions persist only the legacy
filter arrays and metadata (v6 additionally persists corpus bytes), so
`Index::load` deterministically rebuilds exact/chunk/document statistics from
the loaded corpus when under the cap. Legacy bytes are never reinterpreted as
exact statistics. The resulting prediction bound is the remaining eligible
chunks/blocks and extended verifier-slice bytes; Bloom false positives and
hash collisions may make observations lower than estimates, while
conservative candidate membership and all exact-match semantics remain
unchanged.

## M1.3 PlanKey cross-check

`make_plan_key` copies the pattern expression, all `PatternOptions` fields, all
semantic `SearchOptions` fields (including `record_separator`, binary policy,
max count, invert/files flags, and sorted/deduplicated eligible IDs), the
`IndexOptions` capability fields, and `transformed_input_identity`
   (`pergrep.hpp:130-176`; equality/hash implementation in
   `search.cpp:661-724`).
Distinct keys therefore force a fresh estimate; no plan cache is present in this
milestone. The internal `IndexData::pos_block` is a derived runtime value
(`internal.hpp:240`) rather than an independently hashed PlanKey field. Corpus
frequency arrays, current file metadata, and freshness are inputs to
execution/estimation but are not PlanKey members.

This key is a correctness boundary for future caching, not a promise of
cost-based dispatch. Any future scheduler must preserve the current exact
backend and wrapper semantics documented above, and must keep unsupported
combinations explicit rather than treating an estimate label as an available
plan.

## Source ownership

The implementation evidence for this inventory is:

- `src/search.cpp`: candidate filters, fixed dispatch, wrappers, accounting, and
  cost annotations.
- `src/regex.cpp`: parser, QueryIR extraction, Thompson NFA compiler/search,
  extended VM, and regex match iteration.
- `src/internal.hpp`: index/candidate structures, QueryIR conservativeness,
  `RegexProgram`, `VerifierKind`, and `QueryCost`.
- `include/pergrep/pergrep.hpp`: public pattern/search/index options,
  `PlanKey`, `SearchStats`, and plan-regret data types.

When a scheduler is added, update this document in the same change as its
trigger predicates and correctness gates; do not infer a newly available plan
from cost-model candidate metrics alone.

## Objective-aware execution (M1.8)
All operators default to exhaustive ordered traversal. `FirstHit` and `OrderedPrefix` may stop only on the proven file/offset order; they must not select candidates by estimated hit probability. `SearchStats` records the objective, preserved candidate order, stop reason, cancellation state, and first-hit timing. An absent cancellation callback means no cancellation is requested; multi-worker cancellation is out of scope.
