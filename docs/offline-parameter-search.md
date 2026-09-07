# Bounded Offline Parameter Search (M9.1)

**Status:** Accepted contract
**Scope:** M9.1 / GitHub issue #85
**Owner:** M9 (Offline autotuning & release gates); M1 and M8 own parameter meanings

## Purpose

Explore index representation and planner execution parameters offline without silently mutating runtime defaults or application behavior.
The parameter space search explores bounded candidate spaces across sample documents and queries, recording complete provenance (compiler, target architecture, OS platform, and toolchain).

## Exploration Parameters & Bounds

The parameter space covers:

| Parameter | Type | Default Candidates | Constraint / Invariant |
|---|---|---|---|
| `chunk_bytes` | `std::size_t` | 8192, 16384, 32768, 65536 | Positive chunk boundary |
| `chunk_overlap` | `std::size_t` | 64, 128, 256 | Must be strictly `< chunk_bytes` |
| `positional_block_bytes` | `std::size_t` | 128, 256, 512 | Must be `\le chunk_bytes` |
| `positional_budget_ratio` | `double` | 0.25, 0.50, 0.75 | Valid ratio in `(0.0, 1.0]` |
| `planned_qgrams` | `std::uint64_t` | 0, 1, 2, 4 | Probed count limit |
| `operator_policy` | `std::string` | "default", "scalar", "avx2" | Operator execution family |

## Termination & Resource Bounds

1. **Max Evaluations:** Search stops as soon as `max_evaluations` candidate points have been built and evaluated.
2. **Wall-Clock Budget:** Search terminates when elapsed time exceeds `max_search_time_budget_ms` (default 10,000 ms).
3. **Early Termination / Skip:** Invalid configurations (such as `overlap >= chunk_bytes` or `block_bytes > chunk_bytes`) are recorded in `invalid_configurations_skipped` without spending execution budget.
4. **Reproducibility:** Given identical inputs (corpus, queries, and `ParameterBounds`), the sequence of configurations explored is completely deterministic.

## Toolchain & Platform Provenance

Each candidate and search result captures `toolchain_info` identifying:
- Compiler family and version (Clang, MSVC, GCC)
- Target architecture (x86_64, arm64, arm)
- Operating system (Windows, Linux, macOS)

## Evidence Anchors

- [`include/pergrep/autotune.hpp`](../include/pergrep/autotune.hpp): Public interface (`ParameterBounds`, `TunedCandidate`, `SearchResult`, `explore_parameter_space()`, `current_toolchain_info()`).
- [`src/autotune.cpp`](../src/autotune.cpp): Search loop implementation with bounded resource limits and scoring.
- [`tests/test.cpp`](../tests/test.cpp): `test_m91_bounded_offline_search()`.
