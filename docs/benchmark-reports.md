# Benchmark Reports and Provenance (M9.6)

**Status:** Accepted contract
**Scope:** M9.6 / GitHub issue #90
**Owner:** M9 (Offline autotuning & release gates); all performance contributors attach reports to their leaf issues

## Purpose

Make every performance decision reviewable by collaborators and future maintainers.
Publish machine-readable JSON and human-readable Markdown reports containing full corpus, query, compiler, hardware, cache state, timing, RSS, page faults, candidate/probe counters, p50/p95, fallback rate, and correctness status.

## Evidence Classification

Every report declares an `EvidenceKind` to separate correctness verification from empirical measurement:

| Kind | Purpose | CI Gate |
|---|---|---|
| `CorrectnessSmoke` | Fast smoke test verifying functional behavior | Required for all PR merges |
| `PerformanceEvidence` | Formally benchmarked evidence with full provenance | Required for release candidate promotion |

A result **cannot** be interpreted without its workload and environment provenance; reports explicitly distinguish correctness smoke tests from performance evidence.

## Report Schema & Provenance

Every `BenchmarkReport` contains the following immutable provenance fields:

### Environment & Hardware
- `os_name`, `compiler_name`, `architecture`, `hardware_concurrency`

### Workload & Corpus
- `profile_id` (canonical profile identifier from M9.2), `corpus_seed`, `corpus_files`, `corpus_bytes`
- `cache_state` (`cold` / `warm` / `resident`), `iteration_count`

### Build & Resource
- `build_time_ms`, `load_time_ms`
- `index_bytes`, `peak_rss_bytes`, `page_faults`

### Search Performance
- `aggregate_search_p50_ms`, `aggregate_search_p95_ms`, `fallback_rate`

### Per-Query Breakdown
Per `QueryExecutionRecord`: `pattern_expression`, `query_family`, `p50_latency_ms`, `p95_latency_ms`, `match_count`, `verified_bytes`, `candidate_chunks`, `candidate_blocks`

### Evidence Distinction
`evidence_kind` field distinguishes:
- `correctness_smoke`: Fast smoke tests (CI gate)
- `performance_evidence`: Full benchmark data (release gate)

## Output Formats

1. **Machine-Readable JSON:** Strict schema for CI ingestion, artifact archiving, and automated analysis
2. **Human-Readable Markdown:** Executive tables with provenance summary and query breakdown for human review

## Validation Rules

`validate_provenance()` enforces that no report is interpretable without:
- `report_id`, `profile_id`, `corpus_seed`, `corpus_files`, `corpus_bytes`
- Complete `os_name`, `compiler_name`, `architecture`, `hardware_concurrency`
- At least one query execution record for performance evidence

## Evidence Anchors

- [`include/pergrep/report.hpp`](../include/pergrep/report.hpp): Public interface (`EvidenceKind`, `QueryExecutionRecord`, `BenchmarkReport`, `create_report_from_measurements()`, `to_json()`, `to_markdown()`, `from_json()`, `validate_provenance()`).
- [`src/report.cpp`](../src/report.cpp): JSON/Markdown serialization, provenance validation, and report builder from measurement data.
- [`tests/test.cpp`](../tests/test.cpp): `test_m96_benchmark_reports()`.