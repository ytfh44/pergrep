# Roaring-style container evaluation and decision (M8.3)

**Status:** Accepted decision (format switch rejected; dense baseline retained)
**Scope:** M8.3 / GitHub issue #80
**Owner:** M8; M3 owns persistence compatibility; M8.4 builds on dense SIMD kernels

## Purpose

Decide whether a Roaring-style compressed bitmap family (array, bitset, and run containers)
improves pergrep's query-major index enough to justify a new format version, additional
allocations, and an expanded maintenance surface.

## Container representations evaluated

| Container type | Representation | Memory per row | Best-case workload | Worst-case workload |
|---|---|---|---|---|
| **Array container** | Sorted `std::vector<uint32_t>` | $k \times 4$ B | Very sparse ($k \le 4$, $<1\%$ density) | Dense ($k \to N$, 32x larger than bitset) |
| **Bitset container** | Dense `std::vector<uint64_t>` | $\lceil N/64 \rceil \times 8$ B | Dense ($k > N/16$, $>6.25\%$ density) | Very sparse ($k=1$, fixed overhead) |
| **Run container** | Array of `{start, len}` pairs | $R \times 8$ B | Clustered runs of chunk IDs | Alternating IDs ($R = k$, 2x larger than array) |

## Evaluation results

### 1. Space and memory

- **Hash pseudo-randomness:** 4-gram Murmur-style hashes scatter pseudo-randomly across chunks.
  Consecutive chunk IDs containing the same hash are rare in real-world corpora unless a token
  repeats across every chunk of a single large file. In benchmarks on M0 workloads (50 docs, 23 docs,
  large 1 MB doc), average run length was $1.08$ chunks, meaning Run containers required
  $1.85\times$ more memory than plain Array containers due to storing both start and length.
- **Group sharding:** Pergrep's index already shards chunks into 8 geometric size classes
  ($\lg = 9 \dots 16$), naturally bounding universe size $N$ per group. This keeps dense bitmaps
  compact ($\le 64$ KB per row even for 65536-chunk groups).
- **M8.2 hybrid already captures sparse wins:** M8.2 introduced sparse posting lists for rows
  below the $N/32 \approx 3.125\%$ threshold, achieving the space benefits of array containers
  without multi-container switching complexity.

### 2. Query intersection and lookup performance

- **Bitset intersection:** Word-level bitwise AND (`c[j] &= p[j]`) processes 64 chunks per instruction.
  With M8.4 SIMD vectorization (AVX2/AVX-512/NEON), throughput reaches 256–512 chunks per instruction.
- **Roaring intersection overhead:** Intersecting mixed container types (Array-Bitset, Run-Bitset,
  Run-Array) introduces dynamic dispatch, branches, and temporary conversions that eliminate the
  SIMD pipelining advantage on modern superscalar CPUs.
- **Random access:** Positional verification and candidate validation require constant-time $O(1)$
  bit checks (`test(chunk_id)`). Array binary search takes $O(\log k)$ and Run scan takes $O(R)$,
  increasing cache misses during verifier dispatch.

### 3. Serialization, persistence, and format complexity

- Adopting Roaring containers on-disk would require bumping the snapshot schema from v7 to v8 (M3.2),
  introducing variable-length container headers, alignment padding, and endian-conversion routines
  for each container subtype.
- Build/compaction (M4.5) would require dynamic container type selection and run-length coalescing,
  increasing build times by an estimated 12–18%.

## Decision

**Verdict: Roaring-style compressed containers are NOT justified for pergrep's index format.**

1. **Retain dense bitmaps for persistent v7 format:** Constant-time $O(1)$ membership checks,
   deterministic bounded serialization size, zero decoding branches during scan, and direct
   64-bit alignment for SIMD vectorization (M8.4).
2. **Retain M8.2 hybrid for transient execution:** The two-state sparse/dense model ($N/32$ threshold)
   captures the memory reduction for rare q-grams without the three-way container dispatch overhead.
3. **Scalar fallback contract:** The scalar dense bitmap loop remains the universally compliant
   fallback across all platforms, ensuring identical candidate sets regardless of hardware capabilities.

## Evidence anchors

- [`src/internal.hpp`](../src/internal.hpp): `RoaringGroup` prototype definition and memory accounting.
- [`src/index.cpp`](../src/index.cpp): `roaring_group_array_add`, `roaring_group_bitset_add`, `roaring_group_run_add`.
- [`docs/sparse-vs-dense-qgrams.md`](sparse-vs-dense-qgrams.md): M8.2 analytical threshold baseline.
- [`tests/test.cpp`](../tests/test.cpp): `test_m83_roaring_container_decision`.
