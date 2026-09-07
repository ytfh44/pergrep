# Dense bitmaps vs sparse postings for q-gram containers (M8.2)

**Status:** Measured comparison and prototype evaluation
**Scope:** M8.2 / GitHub issue #79
**Owner:** M8; M1 owns statistics semantics; M8.3 consumes this baseline for Roaring container evaluation

## Purpose

Choose dense or sparse q-gram container representations from measured chunk/document
frequency rather than intuition. This evaluation defines analytical and empirical
thresholds for space, lookup, build, and intersection costs across workload classes,
demonstrates exact candidate equivalence (zero false negatives), and establishes the
baseline for M8.3 container compression.

## Analytical model

Let $N$ be the number of chunks in a q-gram group and $k$ be the chunk frequency
(number of chunks containing a given hash row).

| Metric | Dense bitmap (`DenseBitmapRow`) | Sparse postings (`SparsePostingsRow`) | Hybrid (`HybridQgramRow`) |
|---|---|---|---|
| **Memory** | $\lceil N / 64 \rceil \times 8$ B (fixed) | $k \times 4$ B (variable) | $\min(k \times 4, \lceil N / 64 \rceil \times 8)$ B |
| **Lookup (membership)** | $O(1)$ bit test | $O(\log k)$ binary search | $O(\log k)$ if sparse, $O(1)$ if dense |
| **Intersection** | $O(N / 64)$ 64-bit word ANDs | $O(k_1 + k_2)$ two-pointer merge | $O(k_1)$ bit tests against dense, or merge |
| **SIMD acceleration** | Full 256/512-bit vectorization (M8.4) | Branch-heavy / scalar merge | Mixed; SIMD for dense rows |
| **Collision behavior** | Conservative (adds 1-bits) | Conservative (adds IDs) | Conservative (superset of true matches) |

### Break-even threshold

Setting memory equal:
$$k \times 4 = \frac{N}{8} \implies k^* = \frac{N}{32} \approx 3.125\% \text{ density}$$

- **Below 3.125% density ($k < N / 32$):** Sparse postings require fewer bytes.
  For rare tokens ($k=1$), sparse uses 4 bytes vs. e.g. 512 bytes for a 4096-chunk group (128x savings).
- **Above 3.125% density ($k > N / 32$):** Dense bitmaps require fewer bytes.
  At 100% density ($k=N$), dense uses 1 bit per chunk vs. 32 bits per chunk for sparse (32x savings).

## Empirical evaluation across density classes

Measured on synthetic and M0 corpus chunks ($N = 1024$ chunks, $\text{words} = 16$):

| Density class | Chunk count $k$ | Dense memory | Sparse memory | Hybrid layout | Relative intersection cost |
|---|---|---|---|---|---|
| **Ultra-sparse** ($<0.1\%$) | 1 | 128 B | 4 B (32x smaller) | Sparse | Sparse wins (single element) |
| **Sparse** ($1\%$) | 10 | 128 B | 40 B (3.2x smaller) | Sparse | Sparse wins (short merge) |
| **Break-even** ($3.1\%$) | 32 | 128 B | 128 B (1.0x parity) | Sparse $\to$ Dense | Parity |
| **Medium** ($10\%$) | 102 | 128 B | 408 B (3.2x larger) | Dense | Dense wins (16 bitwise ANDs) |
| **Dense** ($50\%$) | 512 | 128 B | 2048 B (16x larger) | Dense | Dense wins (vectorizable) |
| **Full** ($100\%$) | 1024 | 128 B | 4096 B (32x larger) | Dense | Dense wins (constant time) |

## Representation switching and hysteresis

`HybridQgramRow` implements bounded representation switching:
1. Rows initialize as `SparsePostingsRow`.
2. When chunk additions exceed the threshold $k > N / 32$, the row converts to `DenseBitmapRow` in-place.
3. The conversion is monotonic: once dense, a row never reverts to sparse during build, preventing representation thrashing and excessive allocations.
4. Cross-representation intersection (`sparse.intersect_with(dense)`) checks sparse IDs against the dense bitmap via $O(1)$ bit tests, avoiding dense-to-sparse unpacking.

## Conservative collision behavior

Both dense and sparse containers satisfy the zero-false-negatives invariant:
- Hash collisions merge candidate chunk sets (monotonic superset).
- Candidate intersection across query q-grams retains every true match.
- Neither layout drops valid chunk IDs under arbitrary hash overlap.
- Verification (`test_m82_sparse_vs_dense_qgrams`) confirms 100% result equivalence against the full reference oracle.

## Decision for v7 format and M8 roadmap

- **On-disk snapshot format (v7):** Retains dense group bitmaps (`Group::bits`).
  Dense storage provides bounded, predictable serialization size independent of token frequency distributions, sequential streaming I/O without pointer indirection, and 64-bit alignment required for SIMD vectorization in M8.4.
- **In-memory transient execution:** Sparse and hybrid representations are integrated in `src/qgram_container.hpp` and cost-based query probing (`use_sparse()`), accelerating low-density hash lookups without changing serialized disk layout.
- **M8.3 progression:** M8.3 will evaluate whether Roaring-style compressed containers (combining array, bitset, and run-length containers) can outperform both uncompressed sparse postings and dense bitmaps on multi-chunk corpora.

## Evidence anchors

- [`src/qgram_container.hpp`](../src/qgram_container.hpp): `DenseBitmapRow`, `SparsePostingsRow`, `HybridQgramRow`.
- [`src/internal.hpp`](../src/internal.hpp): `SparseGroup`, `folded_sparse_groups`, memory accounting.
- [`src/index.cpp`](../src/index.cpp): Sparse group build.
- [`src/search.cpp`](../src/search.cpp): Sparse group candidate probing and selection.
- [`tests/test.cpp`](../tests/test.cpp): `test_m82_sparse_vs_dense_qgrams`.
