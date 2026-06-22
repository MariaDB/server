# MariaDB - DuckDB Incompatibilities

Discovered during porting the DuckDB storage engine plugin to MariaDB 12.
DuckDB upstream version: **v1.5.2** (submodule at `third_parties/duckdb/`).

---

## 1. Identifier Quoting

| MariaDB | DuckDB |
|---|---|
| `` `backticks` `` | `"double quotes"` (SQL standard) |

MariaDB uses backticks by default for quoting identifiers. DuckDB follows the SQL standard and uses double quotes.

**Fix applied**: `backticks_to_double_quotes()` in `duckdb_query.cc` converts every backtick to a double quote in all SQL sent to DuckDB via `duckdb_query()`. DDL/DML codegen in `ddl_convertor.cc`, `dml_convertor.cc`, `ha_duckdb.cc`, `delta_appender.cc` uses double quotes directly.

SELECT pushdown (`ha_duckdb_pushdown.cc`) uses the original SQL text from `THD::query()` (which contains backticks), so the conversion happens inside `duckdb_query()`.

---

## 2. Function Compatibility

MariaDB function semantics differ from DuckDB in several areas. These are handled by **runtime function overrides** registered at startup via `register_mysql_compat_functions()` in `duckdb_mysql_compat.cc`. No DuckDB source patches are used.

### Overridden functions (registered in `duckdb_mysql_compat.cc`)

| Function | Issue | Fix |
|---|---|---|
| `octet_length(VARCHAR)` | DuckDB builtin only has BLOB overload | Added VARCHAR→BIGINT overload (byte count) |
| `length(VARCHAR)` | DuckDB returns character count; MariaDB returns byte count | Overridden to return byte count |
| `length(BLOB)` | DuckDB builtin `length()` only works on VARCHAR | Added BLOB→BIGINT overload |
| `ascii(VARCHAR)` | DuckDB returns Unicode codepoint; MariaDB returns first byte value | Overridden to return first byte |
| `ord(VARCHAR)` | DuckDB returns Unicode codepoint; MariaDB returns multibyte byte-value | Overridden for MariaDB semantics |
| `hex()` | DuckDB builtin missing several type overloads | Full overload set (VARCHAR, BLOB, BIGINT, UBIGINT, HUGEINT, UHUGEINT, DOUBLE, FLOAT) |
| `oct()` | Not in DuckDB | Full implementation with all numeric + string overloads |
| `bin()` | Not in DuckDB | Full implementation with all numeric + string overloads |
| `locate(substr, str [, pos])` | DuckDB has `instr(str, substr)` with reversed arg order | Custom 2-arg and 3-arg implementations |
| `mid(s, p [, n])` | Not in DuckDB | SQL macro delegating to `substr()` |
| `addtime(TIMESTAMP/TIME, VARCHAR)` | DuckDB INTERVAL doesn't parse MariaDB `'D H:M:S.us'` format | Custom implementation parsing MariaDB interval strings |
| `subtime(TIMESTAMP/TIME, VARCHAR)` | Same as addtime | Custom implementation |
| `rtrim(VARCHAR, VARCHAR)` | DuckDB removes individual chars from set; MariaDB removes substring | Overridden for substring semantics |
| `ltrim(VARCHAR, VARCHAR)` | Same as rtrim | Overridden for substring semantics |
| `regexp_instr(VARCHAR, VARCHAR)` | Not in DuckDB | Custom implementation using RE2 |
| `regexp_replace(VARCHAR, VARCHAR, VARCHAR)` | DuckDB has it but with different overload set | Custom 3-arg implementation using RE2 |
| `regexp_substr(VARCHAR, VARCHAR)` | Not in DuckDB | Custom implementation using RE2 |
| `json_unquote(VARCHAR)` | Not in DuckDB | Custom implementation (strip quotes + unescape) |
| `json_contains(VARCHAR, VARCHAR, VARCHAR)` | DuckDB only has 2-arg form | 3-arg placeholder (returns false — needs proper implementation) |

### Compatible aliases (already work in DuckDB)

| User writes | MariaDB canonical | DuckDB status |
|---|---|---|
| `LOWER()` | `lcase()` | Alias exists |
| `UPPER()` | `ucase()` | Alias exists |
| `IFNULL()` / `NVL()` | `ifnull()` | Rewritten to `COALESCE` |
| `CEIL()` | `ceiling()` | Supported natively |
| `POWER()` | `pow()` | Supported natively |
| `SUBSTRING()` | `substr()` | Supported natively |

### Potentially incompatible (not yet triggered)

| User writes | MariaDB canonical | DuckDB status |
|---|---|---|
| `SCHEMA()` | `database()` | No `database()` function in DuckDB |
| `ATAN2(x,y)` | `atan(x,y)` | Arity may differ |

---

## 3. SQL Syntax Rewriting (SELECT pushdown)

SELECT pushdown uses the original SQL text from `THD::query()`. MariaDB-specific syntax is rewritten in `ha_duckdb_pushdown.cc` (`init_scan()`) before sending to DuckDB:

| MariaDB syntax | DuckDB equivalent | Status |
|---|---|---|
| `GROUP BY ... WITH ROLLUP` | `GROUP BY ROLLUP(...)` | Rewritten |
| `CONVERT(expr, TYPE)` | `CAST(expr AS TYPE)` | Rewritten |
| `CURRENT_TIME()` / `CURRENT_DATE()` / `CURRENT_TIMESTAMP()` | `current_time` / `current_date` / `current_timestamp` (keywords) | Rewritten (parens removed) |
| `STRAIGHT_JOIN` | `CROSS JOIN` | Rewritten |
| Conditionless `JOIN` (no ON/USING) | `CROSS JOIN` | Rewritten |
| `REGEXP` / `NOT REGEXP` / `RLIKE` / `NOT RLIKE` | `~` / `!~` | Rewritten |
| `LIMIT offset, count` | `LIMIT count OFFSET offset` | Rewritten |
| `HIGH_PRIORITY`, `SQL_NO_CACHE`, `SQL_CACHE`, `SQL_BUFFER_RESULT`, `SQL_SMALL_RESULT`, `SQL_BIG_RESULT`, `SQL_CALC_FOUND_ROWS` | -- | Stripped |
| `FORCE INDEX(...)`, `USE INDEX(...)`, `IGNORE INDEX(...)` | -- | Stripped |

### Known unhandled cases (currently cause query failures)

These MariaDB constructs are **not yet rewritten** and fail when pushed down. Because pushdown forwards the original `THD::query()` text (only backticks are converted to double quotes), MariaDB-specific token semantics survive into DuckDB. Discovered while running an analytical query set (402 queries) against DuckDB-engine tables.

| MariaDB construct | Sent to DuckDB as | DuckDB result | Root cause |
|---|---|---|---|
| Double-quoted **string literal**, e.g. `JSON_OBJECT("month", ...)` | `"month"` (verbatim) | `Binder Error: Referenced column "month" not found` | MariaDB without `ANSI_QUOTES` treats `"x"` as a string literal; DuckDB treats `"x"` as an identifier. The forwarded literal is read as a column reference. |
| Unquoted column **alias equal to a DuckDB reserved keyword**, e.g. `SELECT expr name` / `SELECT expr year` | `... name` / `... year` (verbatim) | `Parser Error: syntax error at or near "name"` | DuckDB forbids reserved keywords as unquoted identifiers. `AS name` or `"name"` work; bare `name` / `year` / `month` do not. This is why most implicit aliases pass but keyword aliases fail. |

Reproductions (against any DuckDB-engine table `t`):

```sql
SELECT JSON_OBJECT("k", 1) FROM t;   -- Binder Error: column "k" not found
SELECT JSON_OBJECT('k', 1) FROM t;   -- OK
SELECT col name FROM t;              -- Parser Error at "name"
SELECT col AS name FROM t;           -- OK
```

**Fix direction**: in `ha_duckdb_pushdown.cc`, convert double-quoted string literals to single-quoted form and quote (or `AS`-prefix) aliases that are DuckDB reserved keywords. Both require lexer-aware handling of the query text, not naive replacement — `backticks_to_double_quotes()` already produces legitimate double-quoted identifiers that must not be altered.

---

## 4. Data Type Handling

### DECIMAL Appender

DuckDB upstream Appender API does not accept raw `Append<intN_t>()` for DECIMAL columns -- it interprets them as plain integers and fails the type cast. The fix is to use `duckdb::Value::DECIMAL(value, width, scale)` which tells DuckDB the value is already scaled.

**Affected file**: `delta_appender.cc`

### Text types

MariaDB `MEDIUMTEXT`, `LONGTEXT`, `TEXT`, `TINYTEXT` all map to DuckDB `VARCHAR`.

---

## 5. DuckDB API Differences (AliSQL fork vs upstream v1.5.2)

The AliSQL fork of DuckDB has custom extensions to the API that do not exist in upstream DuckDB:

| Feature | AliSQL fork | Upstream v1.5.2 |
|---|---|---|
| `scheduler_process_partial` config option | YES | NO - Does not exist |
| `appender_allocator_flush_threshold` config option | YES | NO - Does not exist |
| `Appender(conn, schema, table, AppenderType)` constructor | YES | NO - No `AppenderType` parameter |
| `LengthFun` for VARCHAR | Uses `StrLenOperator` (byte count) | Uses `StringLengthOperator` (codepoint count) — overridden at runtime via `duckdb_mysql_compat.cc` |

---

## 6. DuckDB Extensions

Loaded via `cmake/duckdb_extensions.cmake`:

| Extension | Required for | Explicitly loaded? |
|---|---|---|
| **ICU** | `SET TimeZone = ...`, locale-aware collations | YES |
| **JSON** | JSON functions | YES |
| **core_functions** | Basic scalar/aggregate functions | YES |

Build flags in `cmake/duckdb.cmake`: `EXTENSION_STATIC_BUILD=1`, `DUCKDB_EXTENSION_AUTOLOAD_DEFAULT=1`, `DUCKDB_EXTENSION_AUTOINSTALL_DEFAULT=1`.

---

## 7. ABI Compatibility

DuckDB is built as a static library (`libduckdb_bundle.a`) via `ExternalProject_Add` in `cmake/duckdb.cmake`.

| Concern | Detail |
|---|---|
| **`_GLIBCXX_DEBUG`** | MariaDB debug builds define `_GLIBCXX_DEBUG` which changes `sizeof(std::vector)` etc. The plugin target strips this via `-U_GLIBCXX_DEBUG -U_GLIBCXX_ASSERTIONS` in `CMakeLists.txt`. Mismatch causes SIGSEGV in `~DuckDB()`. |
| **C++ standard** | Plugin is built with C++17 (`CXX_STANDARD 17`). DuckDB v1.5.2 requires C++17. |
| **`_GLIBCXX_USE_CXX11_ABI`** | Not explicitly set — uses the compiler default (ABI=1). Both DuckDB and the plugin use the same default. |

---

## 8. Potential Future Incompatibilities

These have not been triggered yet but are likely to cause issues when more complex queries are pushed down:

| MariaDB function | DuckDB equivalent | Notes |
|---|---|---|
| `GROUP_CONCAT()` | `string_agg()` / `list_aggr()` | Different syntax and separator handling |
| `DATE_FORMAT()` | `strftime()` | Different format specifiers |
| `UNIX_TIMESTAMP()` | `epoch()` | -- |
| `FORMAT(number, decimals)` | -- | MariaDB: locale-aware number formatting; DuckDB: printf-style |
| `FOUND_ROWS()` | -- | No equivalent |
| `LAST_INSERT_ID()` | -- | No equivalent |
| Collations | -- | MariaDB collation rules do not transfer to DuckDB |
