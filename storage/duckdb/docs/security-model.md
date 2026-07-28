# Security Model: DuckDB and the `run_in_duckdb` Function

This document describes the security characteristics of the embedded DuckDB
instance used by the engine, the limitations of DuckDB's security model, and
the security side effects of the `run_in_duckdb()` SQL function. It is intended
for operators deciding whether and where to enable the engine, especially in
shared or multi-tenant deployments.

> **Summary:** `run_in_duckdb()` executes arbitrary DuckDB SQL, in-process,
> as the server OS user, on a single shared DuckDB instance. It is gated by two
> coarse checks: the `duckdb_allow_run_in_duckdb` system variable must be ON
> (it is **OFF by default**), and the caller must hold the `SUPER` privilege.
> DuckDB itself applies no access control. Treat the ability to call it as
> equivalent to granting `FILE` plus arbitrary code-adjacent access on the
> server host, and do not enable it in untrusted or multi-tenant environments.

## 1. DuckDB's security model and its limitations

DuckDB is an *embedded* analytical database. Its threat model assumes the
embedding application is trusted and is responsible for access control. As a
result:

- **No users, roles, or object privileges.** DuckDB has no `GRANT`/`REVOKE`
  system comparable to a server DBMS. There is no per-table, per-column, or
  per-row authorization inside DuckDB. Any SQL that reaches the DuckDB
  connection runs with full rights over the whole DuckDB catalog.
- **Filesystem and network access are SQL-level features.** DuckDB SQL can read
  and write local files (`COPY ... TO`, `read_csv`, `read_parquet`,
  `read_text`, `ATTACH`) and — once the relevant extensions are present — reach
  the network (e.g. `httpfs`). These are normal SQL functions, not privileged
  operations.
- **Security is configuration-driven and must be locked.** DuckDB's intended
  hardening mechanism is instance-creation configuration that is then frozen:
  `enable_external_access`, `allowed_directories` / `allowed_paths`,
  `autoinstall_known_extensions`, `autoload_known_extensions`,
  `allow_community_extensions`, `allow_unsigned_extensions`, and
  `lock_configuration`. If these are not set (and locked), the full SQL surface
  is available.

See the upstream guidance: <https://duckdb.org/docs/operations_manual/securing_duckdb/overview>.

## 2. How `run_in_duckdb()` works

`run_in_duckdb(sql_string)` is registered as a `MariaDB_FUNCTION_PLUGIN`
alongside the storage engine (`duckdb_udf.cc`, `ha_duckdb.cc`). When called:

0. It enforces two access checks before doing anything else:
   - if the `duckdb_allow_run_in_duckdb` system variable is OFF (the default),
     it fails with `ER_OPTION_PREVENTS_STATEMENT`
     (`--duckdb-allow-run-in-duckdb`);
   - otherwise it requires the caller to hold the global `SUPER` privilege
     (`check_global_access(thd, SUPER_ACL)`); non-`SUPER` callers get an
     access-denied error and a `NULL` result.
1. It takes the single string argument **verbatim** (the only transformation is
   that backticks are rewritten to double quotes for identifier quoting).
2. It opens a connection to the **single, server-wide** DuckDB instance
   (`DuckdbManager::CreateConnection()`), backed by `duckdb.db` in the data
   directory.
3. It executes the string with `connection.Query()` and returns the rendered
   result (or the DuckDB error text) as a binary string.

Two properties matter for security:

- **Coarse privilege gating only.** Callers must pass the two checks above
  (`duckdb_allow_run_in_duckdb=ON` plus `SUPER`). There is still no *dedicated*
  privilege: `SUPER` is a broad server-admin capability, so gating is all-or-
  nothing rather than per-function or per-object. Any `SUPER` account can call
  it once the sysvar is enabled.
- **No schema scoping.** Unlike normal engine query paths, the function does
  **not** call `config_duckdb_env()`/`config_duckdb_session()`, so the statement
  is not confined to the caller's current database — it runs against the entire
  shared DuckDB catalog.

## 3. Current instance configuration

The shared instance is created in `DuckdbManager::Initialize()` with
performance-oriented settings (memory limit, threads, temp directory,
checkpoint threshold). Relevant to security:

- `enable_external_access` is **left at its default (enabled)** — filesystem
  access is permitted.
- `autoload_known_extensions=true` and `autoinstall_known_extensions=true` are
  **explicitly enabled**, so known extensions can be fetched and loaded on
  demand.
- No `allowed_directories`, `allow_community_extensions=false`, or
  `lock_configuration` restrictions are applied.

These are reasonable defaults for a single-tenant analytical box, but they leave
the DuckDB SQL surface fully open to anyone who can reach it via
`run_in_duckdb()` — i.e. any `SUPER` account once
`duckdb_allow_run_in_duckdb` is enabled.

## 4. Security side effects of `run_in_duckdb()`

Because the function runs arbitrary DuckDB SQL, in-process, as the server OS
user, on the shared instance, a caller can:

- **Bypass the MariaDB privilege system.** Reads and writes to any
  `ENGINE=DuckDB` table (and the whole DuckDB catalog) are possible regardless
  of MariaDB table/column/row grants, because authorization is checked by
  MariaDB only for normal statements — not for SQL executed inside DuckDB.
- **Read arbitrary local files** the server process can access
  (e.g. `SELECT * FROM read_text('/etc/passwd')`,
  `read_csv(...)`), enabling data exfiltration.
- **Write arbitrary local files** as the server user
  (e.g. `COPY (...) TO '/path/file'`), which can corrupt data, fill disks, or
  serve as a step toward code execution.
- **Reach the network** via auto-installable extensions such as `httpfs`,
  enabling SSRF and exfiltration to remote endpoints.
- **Attach external databases** (`ATTACH`) and read/modify their contents.
- **Change global DuckDB settings** on the shared instance (e.g. `SET GLOBAL
  memory_limit`/`threads`), enabling resource-exhaustion / denial of service
  that affects all sessions.

Note that MariaDB's usual file-access guards do **not** constrain these paths:
neither the `FILE` privilege nor `secure_file_priv` is consulted for I/O
performed inside DuckDB.

## 5. What is *not* enforced

- No *dedicated* (per-function/per-object) privilege: access is gated only by
  `duckdb_allow_run_in_duckdb` plus the broad `SUPER` privilege.
- No mapping of MariaDB users/grants onto DuckDB objects.
- No `secure_file_priv` / `FILE`-privilege enforcement for DuckDB file I/O.
- No restriction on extension install/load or on external (file/network) access.
- No per-user or per-schema isolation: a single DuckDB instance is shared by the
  whole server.

## 6. Recommendations

For operators:

- **Do not enable the engine in untrusted or multi-tenant environments** where
  untrusted `SUPER` accounts could call `run_in_duckdb()`. Treat the function as
  a highly privileged, host-level capability.
- **Keep `duckdb_allow_run_in_duckdb=OFF`** unless you actively need the
  function; it is off by default. Enabling it exposes the full DuckDB SQL
  surface to every `SUPER` account.
- **Restrict who holds `SUPER`.** Because gating is coarse (`SUPER`, not a
  per-function privilege), access control must be applied at the account level
  by limiting `SUPER` grants to trusted operators.
- **Run the server as a least-privileged OS user** and confine it (containers,
  systemd sandboxing, restrictive filesystem permissions), since DuckDB file I/O
  inherits the server process's rights.
- **Isolate the data directory** so that files reachable by the server user do
  not include secrets unrelated to the database.

`run_in_duckdb()` is now gated behind the `duckdb_allow_run_in_duckdb` sysvar
(OFF by default) and the `SUPER` privilege. Further engine hardening remains
unimplemented (out of scope for this document but worth tracking): applying and
locking DuckDB security configuration at instance creation —
`enable_external_access=false`, disabling extension autoinstall/autoload,
`allow_community_extensions=false`, `allowed_directories`, and
`lock_configuration=true` — and/or replacing the coarse `SUPER` check with a
dedicated per-function privilege.

## References

- DuckDB — Securing DuckDB:
  <https://duckdb.org/docs/operations_manual/securing_duckdb/overview>
- `duckdb_udf.cc` — `run_in_duckdb()` implementation and access checks
- `ha_duckdb.cc` — `duckdb_allow_run_in_duckdb` system variable
- `runtime/duckdb_manager.cc` — shared instance configuration
- `runtime/duckdb_query.cc` — query execution path
