# Collation Mapping: MariaDB → DuckDB

## Overview

MariaDB and DuckDB have fundamentally different collation systems. MariaDB uses per-charset collations based on UCA (Unicode Collation Algorithm) weight tables at specific Unicode versions. DuckDB stores all strings as UTF-8 internally and provides collation via two mechanisms:

- **Built-in collations** — `NOCASE` (applies `lower()`), `NOACCENT` (strips diacritics), `NFC` (Unicode normalization). These are combinable: `NOCASE.NOACCENT`.
- **ICU locale collations** (from the ICU extension) — one per ICU locale (e.g. `de`, `en_us`, `fr`, `zh`). These use proper UCA via ICU but are not combinable with the built-in collations.

Additionally, DuckDB registers `icu_noaccent` with the ICU tag `und-u-ks-level1-kc-true` (primary strength + case level), providing accent-insensitive, case-insensitive comparison via the full UCA algorithm.

## Current Mapping

The mapping is implemented in `duckdb_charset_collation.cc` (`get_duckdb_collation()`). It is applied in two places:

1. **DDL** (`ddl_convertor.cc`) — VARCHAR columns get `COLLATE <collation>` appended based on the column's `CHARSET_INFO`.
2. **Session context** (`duckdb_context.cc`) — each DuckDB connection's `default_collation` is set from the MariaDB session's `collation_connection`.

### Supported charsets

Only `utf8mb3`, `utf8mb4`, and `ascii` charsets are mapped to ICU-aware collations. All other charsets (e.g. `latin1`, `cp1251`) fall back to `POSIX` (binary comparison) because DuckDB cannot replicate their byte-level sorting rules.

### Mapping table

The mapping uses `CHARSET_INFO` flags — specifically `MY_CS_BINSORT`, `MY_CS_LOWER_SORT`, and `levels_for_order` bits — to determine the collation behavior.

| MariaDB collation type | `levels_for_order` bits | DuckDB collation | Example MariaDB collation |
|---|---|---|---|
| `_bin` | (binsort flag) | `POSIX` | `utf8mb4_bin` |
| `_ai_ci` | primary only | `NOCASE.NOACCENT` | `utf8mb4_general_ci`, `utf8mb4_0900_ai_ci` |
| `_as_ci` | primary + secondary | `NOCASE` | `utf8mb4_0900_as_ci` |
| `_as_cs` | primary + secondary + tertiary | `POSIX` | `utf8mb4_0900_as_cs` |
| `_tolower_ci` | (lower_sort flag) | `NOCASE` | `utf8mb3_tolower_ci` |
| Non-UTF8 charset | — | `POSIX` | `latin1_swedish_ci` |

### Constants

Defined in `duckdb_charset_collation.h`:

```
COLLATION_BINARY        = "POSIX"
COLLATION_NOCASE        = "NOCASE"
COLLATION_NOCASE_NOACCENT = "NOCASE.NOACCENT"
```

`POSIX` is used instead of `binary` because `binary` is a reserved keyword in DuckDB.

## Known Gaps

### 1. Built-in `NOCASE` vs UCA case folding

DuckDB's `NOCASE` applies `lower()` — simple Unicode case folding. MariaDB's UCA-based `_ci` collations use weight tables that handle complex cases (e.g. `ß` = `ss` in German, `İ` ≠ `I` in Turkish). This means comparison results can differ for non-ASCII characters.

### 2. Built-in `NOACCENT` vs UCA accent handling

DuckDB's `NOACCENT` uses `strip_accents()` which removes combining diacritical marks. MariaDB's `_ai` collations use UCA primary weights which may group characters differently (e.g. ligatures like `æ`).

### 3. No `_as_cs` UCA equivalent

MariaDB's `_as_cs` collations use full UCA tertiary weights for ordering. DuckDB's `POSIX` (binary) preserves case and accent sensitivity but produces a different sort order — it sorts by UTF-8 byte values, not UCA weights. This affects `ORDER BY` results.

### 4. Non-UTF8 charsets lose collation semantics

Any charset other than `utf8mb3`/`utf8mb4`/`ascii` is mapped to binary comparison. MariaDB's `latin1_swedish_ci` ordering (which treats `ä` = `a`, `ö` = `o`, etc.) is lost.

## Possible Improvements

### Use ICU locale collations for better UCA fidelity

DuckDB's ICU extension registers collations for all available ICU locales. The relevant ICU collation tags for MariaDB equivalence are:

| UCA behavior | ICU tag | DuckDB collation |
|---|---|---|
| accent-insensitive, case-insensitive (`_ai_ci`) | `und-u-ks-level1-kc-true` | `icu_noaccent` (already registered) |
| accent-sensitive, case-insensitive (`_as_ci`) | `und-u-ks-level2` | Not yet registered |
| accent-sensitive, case-sensitive (`_as_cs`) | `und-u-ks-level3` | Not yet registered |

Registering `und-u-ks-level2` and `und-u-ks-level3` as custom DuckDB collations and mapping to them would provide much closer UCA equivalence for UTF-8 collations.

### Register MariaDB-named collations

An alternative approach is to register collations with MariaDB-compatible names (e.g. `utf8mb4_0900_ai_ci`) that delegate to the corresponding ICU collator. This would allow the DDL converter to emit the original collation name and avoid translation entirely.
