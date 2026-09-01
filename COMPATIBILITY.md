# ripgrep compatibility contract

## Target

The `pergrep` executable targets the public command-line interface of **ripgrep 15.2.0**.

Compatibility means that scripts can use ripgrep-style options, aliases, negations, mode precedence, filtering and output modes without pergrep delegating search to ripgrep or another grep implementation. The search engine remains the internal `libpergrep` engine.

## Covered behavior

The release conformance tests exercise:

- all ripgrep 15.2.0 logical long flags represented by the compatibility parser, their principal negated/legacy aliases, and all short options;
- last/partial override rules for context, passthru, search/output modes, case/boundary/binary modes and multiline/stop-on-nonmatch;
- fixed and regex search, Unicode case/smart-case, Unicode classes/properties, scoped flags, captures and `-P` backreferences/lookaround;
- stdin haystacks, `-f -`, text encodings and UTF-16 BOM handling;
- nested `.gitignore`, `.ignore`, `.rgignore`, explicit ignore files, parent/global/exclude/VCS sources and their independent disable flags;
- hidden files, symlink following, maximum depth/size and one-file-system filtering;
- built-in and user-defined file types;
- line/context/vimgrep/only-matching/count/files/quiet/invert modes and exit statuses;
- CRLF and NUL-data logical records;
- JSON lines, stats, replacement, invalid-UTF-8 byte objects, colors, `--colors` palette overrides and hyperlinks;
- path/metadata sorting and path separators;
- Auto/SearchAndSuppress/AsText binary behavior for the normal non-mmap path;
- `--pre`, libarchive-backed `-z/--search-zip`, and `--generate` output.

The test suite also contains expected-output cases adapted from ripgrep's public `feature.rs`, `multiline.rs` and binary behavior tests.

## Deliberate engine-policy differences

Some ripgrep options select implementation strategies rather than changing the logical result. `pergrep` accepts them to preserve the CLI contract but maps them to equivalent internal policy:

- `--threads/-j`: accepted; the current executor decides its own execution strategy and is presently single-process/synchronous.
- `--mmap/--no-mmap`: accepted as an execution hint. pergrep does not reproduce ripgrep's mmap-specific binary-detection quirks; normal ripgrep binary semantics are implemented.
- `--dfa-size-limit` and `--regex-size-limit`: accepted as compatibility resource hints; the internal NFA/VM has different memory structures from ripgrep's regex-automata backend.
- buffering options change flushing policy rather than match selection; content semantics are identical.

These are implementation-strategy differences, not delegation to a fallback matcher.

## Search-engine independence

Runtime search code does not invoke or link:

- `rg` / ripgrep
- GNU/BSD grep
- ugrep
- Hyperscan
- PCRE2 matching APIs

`--pre` is the sole intentional general-purpose external process facility: it executes the transformer explicitly supplied by the caller and feeds its stdout into `Index::from_documents`. Compressed input is decoded with libarchive and then searched internally.

## Compatibility provenance

Flag names and precedence rules were checked against ripgrep 15.2.0's public flag definitions and public integration tests. The default file-type table is adapted from ripgrep and is covered by its MIT license; see `THIRD_PARTY_NOTICES.md`.
## Persistent index format

New snapshots use portable format v7 with the eight-byte `PERGREP\0` magic, fixed-width fields, and explicit little-endian encoding for every scalar and vector element. The v7 header stores manifest magic, schema 2, and a feature bitset; bit 0 advertises the optional persisted-corpus section. Unknown feature bits and unsupported schemas fail with an incompatible-schema error before payload loading.

The v7 payload is field-by-field: UTF-8 byte strings carry a fixed-width uint64 length, booleans are uint8 values, signed timestamps are int64 values, floating-point fields use their IEEE-754 uint64 bit pattern, and arrays/vectors are length-prefixed and element-encoded. No compiler padding or host byte order is part of v7.

`Index::load()` keeps read compatibility with pre-manifest v5 source-backed and v6 persisted-corpus snapshots. New writes use v7 only. Migration is read-old/write-new: load the legacy snapshot, then save it to produce v7; legacy files are never rewritten in place. The CLI cache key includes the v7/schema-2 format identity so legacy and portable snapshots cannot collide.
