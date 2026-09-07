# Reproducible Corpus Profiles (M9.2)

**Status:** Accepted contract
**Scope:** M9.2 / GitHub issue #86
**Owner:** M9 (Offline autotuning & release gates); workload owners review additions

## Purpose

Make tuning inputs representative and reusable across all supported platforms without overfitting to one synthetic corpus, requiring network downloads during CI, or introducing licensing restrictions.
Each corpus profile is procedurally generated using deterministic pseudo-random generators with fixed seeds.

## Profile Taxonomy & Workload Categories

The suite defines seven distinct categories matching real-world search contexts:

| Category | Description | Primary Characteristic | Target Entropy |
|---|---|---|---|
| **Code** | Realistic source code modules (C++/Python/JS) | Structured syntax, indentation, identifiers, comments | Medium (~4.5 - 5.5 bits) |
| **Logs** | Structured and unstructured server/system logs | Timestamps, log levels (`[ERROR]`, `[INFO]`), repeating request IDs | Low-to-Medium (~3.5 - 4.5 bits) |
| **Generated / Minified** | Bundles, minified JavaScript, compact JSON | High token density, minimal/no newlines, repeating keys | Medium-to-High (~4.8 - 6.0 bits) |
| **Unicode Text** | Multilingual text (CJK, Cyrillic, accented Latin, Emoji) | Multi-byte UTF-8 sequences, non-ASCII boundary transitions | Variable (~4.0 - 6.5 bits) |
| **Binary Heavy** | Binary files, cache files, packed assets | Embedded null bytes (`\0`), raw byte distributions, interspersed strings | High (~6.0 - 7.8 bits) |
| **Large Files** | Multi-megabyte continuous files | Stress testing chunk traversal, block indexing, and boundary crossings | Medium (~4.5 - 5.5 bits) |
| **High Duplication** | Cloned repositories, duplicate branches, vendored code | Near-duplicate files with small edit distances | Lower comparative entropy |

## File Size Distributions

Each profile configures a characteristic size distribution:
- `Uniform`: Fixed target size per file.
- `SmallFiles`: 1 KB to 16 KB (typical of microservices, configuration, headers).
- `MediumFiles`: 16 KB to 128 KB (typical of source modules, documents).
- `Bimodal`: Bimodal distribution combining small files with occasional massive files.
- `LongTail`: Pareto / power-law distribution with heavy right tail.

## Shannon Entropy Measurement

The framework computes formal Shannon entropy metrics over byte and q-gram frequencies:
$$H(X) = -\sum_{i} P(x_i) \log_2 P(x_i)$$
- **Byte Entropy ($H_{\text{byte}}$):** Range $[0, 8]$ bits/byte, measuring uncompressed byte diversity.
- **Q-Gram Entropy ($H_{\text{qgram}}$):** Range $[0, 32]$ bits per $q$-gram ($q=4$), measuring sequence predictability and filter sparsity.

## Determinism & License Safety

1. **Procedural Generation:** All files are generated purely in memory via fixed-seed pseudo-random generation.
2. **Zero Network Dependence:** CI runners and air-gapped systems generate identical corpora without downloading external data.
3. **License-Safe:** Synthetic generation avoids embedding third-party copyrighted materials.
4. **Reproducibility:** The exact same document paths, sizes, and byte contents are produced regardless of operating system, CPU architecture, or compiler version.

## Evidence Anchors

- [`include/pergrep/profile.hpp`](../include/pergrep/profile.hpp): Public interface (`CorpusProfileSpec`, `CorpusSummary`, `CorpusCategory`, `FileSizeDistribution`, `generate_profile_corpus()`, `analyze_corpus()`, `calculate_byte_entropy()`).
- [`src/profile.cpp`](../src/profile.cpp): Deterministic generator implementation and canonical profiles catalog.
- [`tests/test.cpp`](../tests/test.cpp): `test_m92_corpus_profiles()`.
