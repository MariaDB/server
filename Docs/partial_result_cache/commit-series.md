# Partial Result Cache for MariaDB Nested Loop Joins - Commit Series

## Current Community Commits

The `support_ptrc` branch contains these community-facing commits:

| Commit | Subject | Purpose |
|---|---|---|
| `7e3afa516d2` | `MDEV-PTRC: Cache repeated nested-loop ref rows` | Adds controls, conservative `JT_REF` eligibility, row-batch replay, runtime bypass, fallback, cleanup, EXPLAIN/trace, optimizer hints, and status counters. |
| `af4390d1f1c` | `MDEV-PTRC: Test nested-loop ref row replay` | Adds sys_vars and focused join MTR coverage for off/on correctness, counter values, hints, rejected shapes, negative entries, and runtime bypass. |
| `HEAD` | `MDEV-PTRC: Document nested-loop ref cache design` | Adds public design, implementation, test, performance, and submission notes. |

The internal development task-board commits were intentionally left out of this
branch to keep the upstream review focused.

## Product Patch File Scope

Product source files:

```text
sql/sql_priv.h
sql/sql_class.h
sql/sys_vars.cc
sql/partial_result_cache.h
sql/partial_result_cache.cc
sql/opt_hints_structs.h
sql/opt_hints.cc
sql/opt_hints_parser.h
sql/opt_hints_parser.cc
sql/sql_select.h
sql/sql_select.cc
sql/mysqld.cc
```

Test files:

```text
mysql-test/suite/sys_vars/t/partial_result_cache_basic.test
mysql-test/suite/sys_vars/r/partial_result_cache_basic.result
mysql-test/suite/sys_vars/r/optimizer_switch_basic.result
mysql-test/main/partial_result_cache_join.test
mysql-test/main/partial_result_cache_join.result
```

Public documentation files:

```text
Docs/partial_result_cache/README.md
Docs/partial_result_cache/high-level-design.md
Docs/partial_result_cache/low-level-design.md
Docs/partial_result_cache/test-report.md
Docs/partial_result_cache/performance-report.md
Docs/partial_result_cache/community-submission.md
Docs/partial_result_cache/commit-series.md
Docs/partial_result_cache/benchmarks/
```

## Suggested Community Patch Shape

For a MariaDB community PR, there are two reasonable shapes.

### Option A: One Feature Commit

Use one squashed commit containing:

- SQL-visible configuration and status variables;
- `JT_REF` executor cache;
- `PRC_JOIN` and `NO_PRC_JOIN` reviewer controls;
- focused MTR tests;
- public docs.

This mirrors the MySQL PTRC community-submission format and keeps the public
review centered on one feature.

### Option B: Short Review Series

Use the current three commits:

1. `MDEV-PTRC: Cache repeated nested-loop ref rows`
2. `MDEV-PTRC: Test nested-loop ref row replay`
3. `MDEV-PTRC: Document nested-loop ref cache design`

This is easier to review incrementally and keeps the executor patch separated
from sys_vars and docs.

## Recommended Squashed Commit Message

Use the full proposed commit message from
`Docs/partial_result_cache/community-submission.md`.

Short subject:

```text
MDEV-PTRC: Add partial result cache for nested-loop ref joins
```

## Pre-Submission Checklist

Run:

```bash
git diff --check
cmake --build build_release --target mariadbd --parallel 16
cd build_release/mysql-test
TMPDIR=/tmp ./mtr sys_vars.partial_result_cache_basic main.partial_result_cache_join \
  --parallel=1 \
  --vardir=/tmp/mariadb-ptrc-final-var \
  --tmpdir=/tmp/mariadb-ptrc-final-tmp \
  --force
```

Confirm staged content:

```bash
git diff --cached --name-only
```

Community process checks:

- MDEV/Jira issue exists before opening the PR.
- PR title uses the `MDEV-xxxxx` prefix.
- Contributor license and GitHub identity are acceptable for MariaDB review.
- Buildbot or CI failures are triaged before requesting final review.

Do not stage:

```text
AGENTS.md
CLAUDE.md
build_debug/
build_release/
.DS_Store
/tmp benchmark artifacts
```

## 9. Commit Message Requirements

Commit message requirements should be maintained here instead of only in
chat history. Future Codex/Claude/agent sessions must check this section
before amending, rebasing, squashing, or preparing a community-facing or
review-facing commit.

- Keep each final commit scoped to the feature or fix being submitted. Do not
  include unrelated local test stabilizations, environment workarounds,
  generated artifacts, build outputs, temporary scripts, or internal
  task-board churn.
- Use a concise MySQL-style subject line: component or WorkLog prefix first,
  then an imperative summary of the change. Keep the subject short enough for
  normal Git review tools.
- Follow the concise MySQL/Google-style body format used by nearby community
  submission branches: short subject, blank line, structured body, and body
  lines wrapped at roughly 72 characters.
- Prefer these body sections when they apply:
  `Problem/Background:`, `Solution:`, `Compatibility/Risk:`, `Test:`, and
  `Known gaps:`.
- The message must explain the feature value or bug impact using verified
  facts. Performance, correctness, stability, or compatibility claims must
  cite the exact workload, test, benchmark, or reproduction that produced the
  evidence.
- Do not overclaim validation. Clearly separate tests that passed, tests that
  were skipped or waived, and gates that were not run. If a benchmark is local
  engineering evidence rather than an official benchmark result, say so.
- Mention accepted exceptions only in the relevant validation context. Do not
  present local environment issues or exact-output formatting exceptions as
  product behavior.
- Before final handoff, verify the commit message with:

```bash
git log -1 --format=%B | awk 'length($0)>72 {print NR ":" length($0) ":" $0}'
git show --name-only --format='%h %s%n%B' HEAD
```

The first command should produce no output unless a deliberately unavoidable
line is documented. Review the second command output for unrelated files,
unrelated topics, generated artifacts, and stale chat-only claims before
submission.
