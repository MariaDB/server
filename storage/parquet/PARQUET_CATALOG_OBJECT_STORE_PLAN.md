# Parquet Catalog + Object Store Plan

## Scope
- This plan covers only the `parquet_catalog.*` and `parquet_object_store.*` slices.
- It assumes the broader handler rework will include real condition pushdown.
- Because of that, the catalog layer must expose predicate-aware scan planning instead of only “load the full active file list”.
- This plan does not cover Stage 3 XA/recovery hooks.
- This plan also does not fold full Iceberg manifest/metadata generation into `parquet_catalog`; that needs a follow-on metadata-writer slice.

## Design correction
- Earlier Stage 1 planning treated `cond_push()` as a safe no-op.
- The new direction is different: handler rework should include real condition pushdown.
- Therefore `parquet_catalog` must be designed around:
  - Iceberg REST capability discovery
  - predicate-aware table scan planning
  - enough file/task metadata to let the handler register only relevant Parquet files in DuckDB

## Source-backed assumptions
- Lakekeeper exposes the Iceberg REST catalog under `/catalog`, while `/management` is a separate API for server and warehouse administration. Source: [Lakekeeper Getting Started](https://docs.lakekeeper.io/getting-started/)
- Iceberg REST clients should begin with `GET /v1/config`, which can return defaults, overrides, and the server’s supported endpoint list. Source: [Iceberg REST Catalog Spec](https://iceberg.apache.org/rest-catalog-spec/), [OpenAPI YAML](https://raw.githubusercontent.com/apache/iceberg/main/open-api/rest-catalog-open-api.yaml)
- Table creation uses `POST /v1/{prefix}/namespaces/{namespace}/tables`. Source: [Iceberg OpenAPI YAML](https://raw.githubusercontent.com/apache/iceberg/main/open-api/rest-catalog-open-api.yaml)
- Table loading uses `GET /v1/{prefix}/namespaces/{namespace}/tables/{table}` and can return table-specific configuration, including credentials or tokens for table access. Source: [Iceberg OpenAPI YAML](https://raw.githubusercontent.com/apache/iceberg/main/open-api/rest-catalog-open-api.yaml)
- Table commits use `POST /v1/{prefix}/namespaces/{namespace}/tables/{table}`. The spec explicitly models commits as requirements plus metadata updates, and `409` is retriable while `500` / `502` / `504` can mean commit-state-unknown. Source: [Iceberg OpenAPI YAML](https://raw.githubusercontent.com/apache/iceberg/main/open-api/rest-catalog-open-api.yaml)
- The current Iceberg REST spec includes `POST /v1/{prefix}/namespaces/{namespace}/tables/{table}/plan` for server-side scan planning. That is the right hook for real condition pushdown when the server advertises support for it. Source: [Iceberg OpenAPI YAML](https://raw.githubusercontent.com/apache/iceberg/main/open-api/rest-catalog-open-api.yaml)
- Lakekeeper supports S3 remote signing and vended credentials, and chooses between them based on storage-profile settings and `X-Iceberg-Access-Delegation`. Source: [Lakekeeper Storage](https://docs.lakekeeper.io/docs/latest/storage/)
- `libcurl` supports AWS SigV4 signing through `CURLOPT_AWS_SIGV4`, which gives us a realistic C++ path for direct S3-compatible access without adding a full AWS SDK dependency. Source: [CURLOPT_AWS_SIGV4](https://curl.se/libcurl/c/CURLOPT_AWS_SIGV4.html)

## Important boundary
- `parquet_catalog` should own REST transport, request/response parsing, capability discovery, and error translation.
- `parquet_object_store` should own direct blob operations like `PUT`, `HEAD`, and `DELETE`.
- Neither module should hide commit-state ambiguity.
- Neither module should directly manipulate MariaDB `Field` objects or MariaDB AST nodes.
- Iceberg metadata-file and manifest generation should stay outside these modules.

## Runtime architecture

### `parquet_catalog`
- Talks only to the Iceberg REST catalog endpoint.
- Uses `GET /v1/config` to discover effective settings and supported endpoints.
- Loads table metadata and table-level config for reads and writes.
- Wraps create/load/commit/scan-plan operations in stable C++ APIs.
- Exposes capability flags so the handler can decide whether:
  - full predicate pushdown is possible
  - only coarse file pruning is possible
  - or it must fall back to a broad scan

### `parquet_object_store`
- Talks directly to S3-compatible object storage.
- Starts with a simple direct-access implementation using `libcurl`.
- Uses streaming upload from local temp files rather than loading whole files into memory.
- Treats rollback deletion as best-effort but returns structured per-object status.
- Keeps auth pluggable so we can start with static or temporary credentials and add remote-signing later.

## Files to add

### `storage/parquet/parquet_catalog.h`
- Declare catalog request/response structs.
- Declare a small `ParquetCatalogClient` API.
- Keep this header free of MariaDB row handling and DuckDB code.

### `storage/parquet/parquet_catalog.cc`
- Implement REST calls with `libcurl`.
- Parse JSON responses.
- Translate Iceberg/Lakekeeper errors into structured internal status objects.

### `storage/parquet/parquet_object_store.h`
- Declare object-store config, auth-mode, and CRUD operations.
- Keep the API generic enough for AWS S3 and S3-compatible stores.

### `storage/parquet/parquet_object_store.cc`
- Implement `PUT`, `HEAD`, and `DELETE`.
- Stream local files into object storage.
- Support path-style and virtual-host-style URL construction.

### Optional internal helpers
- `parquet_http.h/.cc`
  - shared `libcurl` session wrapper
  - header collection
  - request body / response body utilities
- `parquet_json.h/.cc`
  - local JSON encode/decode helpers
  - error extraction helpers

## `parquet_catalog` API plan

### Core data types
- `CatalogClientConfig`
  - base catalog URI
  - warehouse identifier
  - auth token / bearer token
  - optional OAuth credential inputs
  - timeout / retry policy
- `CatalogCapabilitySet`
  - `supports_create_table`
  - `supports_commit_table`
  - `supports_commit_transaction`
  - `supports_scan_planning`
  - `supports_register_table`
- `CatalogNamespaceIdent`
  - normalized namespace parts
  - encoded namespace string
- `CatalogTableIdent`
  - namespace
  - table name
- `CatalogTableConfig`
  - table-level overrides from `loadTable`
  - delegated access settings if returned
- `CatalogTableMetadata`
  - table UUID
  - table location
  - format version
  - current snapshot ID
  - schema summary
  - sort / partition summary
- `CatalogLoadTableResult`
  - config
  - metadata
  - ETag if present
- `CatalogPlanScanRequest`
  - table identifier
  - projected columns
  - predicate IR
  - snapshot/ref inputs
  - pagination controls if the planning endpoint needs them
- `CatalogPlanScanResult`
  - status
  - file scan tasks
  - plan tasks if the server returns staged planning work
  - residual predicate if not everything was pushed down
- `CatalogStatus`
  - status code enum
  - http status
  - retryable flag
  - commit-state-unknown flag
  - human-readable message

### Public operations
- `BootstrapConfig()`
  - call `GET /v1/config`
  - persist defaults, overrides, and endpoint list
- `EnsureNamespace()`
  - `HEAD` / `GET` namespace if available
  - create only when missing
- `CreateTable()`
  - wrap `POST /tables`
  - initially use non-staged create for plain `CREATE TABLE`
- `LoadTable()`
  - wrap `GET /tables/{table}`
  - parse table metadata and table-level config
- `CommitTable()`
  - wrap `POST /tables/{table}`
  - accept a prebuilt commit payload from a future Iceberg metadata writer
- `CommitTransactionIfSupported()`
  - optional Stage 1.5 path if `/transactions/commit` is advertised
  - otherwise explicitly return unsupported
- `PlanTableScan()`
  - wrap `POST /tables/{table}/plan` when the server advertises scan planning
  - otherwise return `unsupported`
- `TableExists()`
  - optional convenience wrapper around `HEAD /tables/{table}`

## `parquet_catalog` implementation phases

### Phase 1: bootstrap and capability discovery
- Implement `BootstrapConfig()`.
- Normalize the catalog base URL once.
- Merge defaults and overrides into one effective config map.
- Parse the returned endpoint list into `CatalogCapabilitySet`.
- Treat missing endpoint lists as the default endpoint set from the Iceberg spec.
- Store enough detail for the handler to know whether real scan planning is available.

### Phase 2: namespace and table lifecycle
- Implement namespace encode/decode helpers.
- Implement `EnsureNamespace()`.
- Implement `CreateTable()`.
- Parse table identifiers and enforce one canonical encoding path.
- Reject malformed or incomplete identifiers early.

### Phase 3: table load and metadata extraction
- Implement `LoadTable()`.
- Parse:
  - table location
  - current snapshot ID
  - format version
  - table UUID
  - schema/partition summaries needed later by the handler and metadata writer
- Capture table-level configuration separately from structural metadata.
- Preserve ETags for later optimistic read refreshes.

### Phase 4: commit wrapper
- Implement `CommitTable()`.
- The method should not construct Iceberg updates itself.
- Instead it should accept:
  - `requirements`
  - `updates`
  - optional expected ETag or snapshot guard inputs
- Map responses carefully:
  - `200`: success
  - `409`: requirements failed, safe to retry after refresh
  - `500` / `502` / `504`: commit state unknown, must be surfaced to handlerton
- Generate and attach idempotency keys for commit calls.

### Phase 5: scan planning for real condition pushdown
- Implement `PlanTableScan()`.
- Accept a neutral predicate representation, not raw MariaDB `Item*`.
- Translate that IR into the planning request format.
- Parse returned file/task metadata into a format the handler can use to:
  - register only needed files in DuckDB
  - preserve residual predicates for in-engine filtering if planning is incomplete
- If scan planning is unsupported:
  - return a structured unsupported status
  - do not silently pretend pushdown happened

## `parquet_object_store` API plan

### Core data types
- `ObjectStoreConfig`
  - endpoint URL
  - region
  - bucket
  - key prefix
  - url style (`path` / `virtual-host`)
  - TLS verify flags
  - auth mode
- `ObjectStoreAuthMode`
  - `client_managed_static`
  - `temporary_credentials`
  - `remote_signing`
- `ObjectStoreCredentials`
  - access key ID
  - secret access key
  - optional session token
  - expiration if temporary
- `ObjectLocation`
  - bucket
  - key
  - fully resolved URL
- `PutObjectRequest`
  - local file path
  - object location
  - content type
  - expected content length
- `HeadObjectResult`
  - exists
  - content length
  - ETag if present
- `ObjectStoreStatus`
  - status code enum
  - http status
  - retryable flag
  - message

### Public operations
- `PutFile()`
  - stream a local file to object storage
- `HeadObject()`
  - verify existence and size after upload
- `DeleteObject()`
  - delete one object
- `DeleteObjectsBestEffort()`
  - helper for rollback cleanup

## `parquet_object_store` implementation phases

### Phase 1: URL and key handling
- Normalize bucket + prefix handling.
- Build object keys under the Iceberg table location rather than inventing warehouse-global paths.
- Keep one canonical path policy for data files, for example:
  - `<table-location>/data/<writer-id>/<flush-id>.parquet`
- Support both path-style and virtual-host-style URLs.

### Phase 2: request execution with `libcurl`
- Implement one shared request executor for:
  - `PUT`
  - `HEAD`
  - `DELETE`
- Capture:
  - response code
  - response headers
  - error body
  - curl transport errors
- Keep retries disabled by default in the executor; let the caller decide.

### Phase 3: authentication
- First implementation path:
  - static or temporary credentials
  - signed with `CURLOPT_AWS_SIGV4`
- Include the session token header when using temporary credentials.
- Keep auth setup in one helper so later remote-signing support does not affect upload/delete call sites.

### Phase 4: streaming upload
- Upload by reading from the local temp file with a curl read callback.
- Do not map the whole file into memory.
- Propagate short reads and file-open failures cleanly.
- Set content length explicitly when known.

### Phase 5: verification and delete
- Implement `HeadObject()` for post-upload verification.
- Verify at minimum:
  - object exists
  - content length matches the local file size
- Implement `DeleteObject()` for rollback cleanup.
- Treat delete-missing as success for rollback purposes.

### Phase 6: remote-signing follow-on
- Keep this out of the first object-store PR unless static or temporary credentials are blocked.
- If we need it later:
  - request table config from the catalog using the appropriate access-delegation mode
  - build unsigned S3 requests locally
  - obtain signing material from Lakekeeper
  - replay the signed request to object storage

## Integration contract with later handler rework

### `create()`
- Handler will call `parquet_catalog.BootstrapConfig()`.
- Handler will call `EnsureNamespace()` if the namespace is allowed to be created automatically.
- Handler will call `CreateTable()`.

### `write_row()` / flush path
- Handler will write a local temp Parquet file.
- Handler will call `parquet_object_store.PutFile()`.
- Handler will optionally call `HeadObject()` to verify the upload.
- Handler will stage:
  - local temp path
  - object path
  - file size
  - record count

### `commit()`
- Handlerton will group staged files by table.
- A future Iceberg metadata-writer slice will build the commit payload.
- Handlerton will call `parquet_catalog.CommitTable()` with that payload.
- If `CommitTable()` returns commit-state-unknown, the handlerton must not clear staged state silently.

### `rnd_init()` with real condition pushdown
- Handler will translate MariaDB predicates into a neutral pushdown IR.
- Handler will call `parquet_catalog.PlanTableScan()`.
- If supported, only planned files/tasks are registered in DuckDB.
- If unsupported, the handler can fall back to a broader file list, but it must not claim full pushdown happened.

## Critical dependency: Iceberg metadata writer
- The Iceberg REST commit endpoint expects metadata updates, not a raw list of Parquet object paths.
- Therefore these two modules alone are not enough for a true append commit.
- The next adjacent slice after this plan should be something like:
  - `parquet_iceberg_metadata.h`
  - `parquet_iceberg_metadata.cc`
- That slice should own:
  - data-file record construction
  - manifest writing
  - metadata JSON writing
  - commit update generation
- `parquet_catalog` should be designed now so that adding that slice later does not change its public transport API.

## Testing plan

### Unit tests
- Catalog URL and namespace encoding
- `/v1/config` response parsing
- endpoint capability discovery
- table metadata extraction
- commit error mapping, especially `409` vs commit-state-unknown
- object URL construction
- object auth setup for static and temporary credentials

### Mocked integration tests
- Catalog calls against a local fake HTTP server that returns:
  - create success
  - load success
  - `409` requirement failure
  - `500` commit-state-unknown
  - scan-planning supported and unsupported responses
- Object store calls against MinIO or another local S3-compatible endpoint:
  - upload
  - head verification
  - delete

### MTR follow-on coverage
- `CREATE TABLE ... ENGINE=PARQUET` registers the table in the catalog
- `INSERT` uploads staged files
- `COMMIT` publishes the Iceberg update
- `ROLLBACK` deletes uploaded objects
- predicate pushdown reads fewer files when `PlanTableScan()` is available

## Exit criteria
- `parquet_catalog` can bootstrap config, create/load tables, and surface commit status correctly.
- `parquet_catalog` exposes a capability-aware `PlanTableScan()` API for real condition pushdown.
- `parquet_object_store` can upload, verify, and delete staged Parquet files through a stable C++ interface.
- Both modules are independent of MariaDB row conversion logic.
- The remaining dependency for real Iceberg commits is isolated to a future metadata-writer slice rather than being hidden inside transport code.
