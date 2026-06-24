# Partial Result Cache for MariaDB Nested Loop Joins

Partial Result Cache (PRC/PTRC) for MariaDB is an experimental
statement-local result cache for repeated nested-loop join inner lookups.

This MariaDB contribution scope is intentionally narrower than the MySQL 8.0
PTRC implementation:

- It targets join-only nested-loop `JT_REF` access.
- It leaves subquery-result caching to MariaDB's existing `subquery_cache`.
- It does not replace MariaDB's join buffer, BNL, BKA, or BKAH framework.
- It is disabled by default through `optimizer_switch=partial_result_cache`.

The feature is useful when a nested-loop join repeatedly probes the same inner
key. On a miss, MariaDB performs the original ref lookup and stores the full
inner row batch for that key. On a later hit, the cached rows are replayed into
the table record buffer and normal `evaluate_join_record()` filtering continues.

Community-facing material in this directory:

- [High Level Design](high-level-design.md)
- [Low Level Design](low-level-design.md)
- [Test Report](test-report.md)
- [Performance Report](performance-report.md)
- [Community Submission Notes](community-submission.md)
- [Commit Series](commit-series.md)
- [Benchmark Reproduction Package](benchmarks/README.md)

Use these documents as follows for a MariaDB community submission:

- `high-level-design.md`: Jira/MDEV design and motivation.
- `low-level-design.md`: implementation notes for code review.
- `test-report.md`: validation summary and remaining coverage.
- `performance-report.md`: local evidence and benchmark protocol.
- `community-submission.md`: PR body, known questions, and checklist.
- `benchmarks/`: scripts and SQL for regenerating raw evidence.
