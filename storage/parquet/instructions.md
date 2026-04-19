## AWS Credentials
Add the access keys: Check groupchat for keys.
Replace placeholders in TWO different spots in `ha_parquet.cc`:
```cpp
con->Query("SET s3_access_key_id='YOUR_AWS_ACCESS_KEY_ID'");
con->Query("SET s3_secret_access_key='YOUR_AWS_SECRET_ACCESS_KEY'");
```

## Build MariaDB with DuckDB

```bash
cd ~/MariaDB2/build-mariadb-duckdb
cmake ../server \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWITH_UNIT_TESTS=OFF \
  -DBISON_EXECUTABLE="$(brew --prefix bison)/bin/bison"
cmake --build . --parallel 4
```

## Initialize the database

```bash
sql/mariadb-install-db \
  --srcdir=../server \
  --defaults-file=~/mariadb-parquet.cnf
```

## Start the server

```bash
sql/mariadbd --defaults-file=~/mariadb-parquet.cnf &
```

## Connect

```bash
client/mariadb --socket=/tmp/mariadb-parquet.sock
```

## CMakeLists.txt

```cmake
FIND_PACKAGE(CURL REQUIRED)

MYSQL_ADD_PLUGIN(parquet
  ha_parquet.cc
  STORAGE_ENGINE
  MANDATORY
)

if(TARGET parquet)
  target_include_directories(parquet PRIVATE
    ${CMAKE_SOURCE_DIR}/storage/duckdb/duckdb/third_parties/duckdb/src/include
  )
  target_link_libraries(parquet PRIVATE
    ${CMAKE_BINARY_DIR}/storage/duckdb/duckdb/duckdb-build/src/libduckdb.dylib
    CURL::libcurl
  )
endif()
```

## LakeKeeper Setup

### 1. Clone and start LakeKeeper

```bash
git clone https://github.com/lakekeeper/lakekeeper
cd lakekeeper/examples/minimal
docker compose up -d
```

### 2. Verify it's running

```bash
curl http://localhost:8181/health
```

### 3. Create the warehouse (one time)

```bash
curl -X POST http://localhost:8181/management/v1/warehouse \
  -H "Content-Type: application/json" \
  -d '{
    "warehouse-name": "default",
    "storage-profile": {
      "type": "s3",
      "bucket": "mariadb-parquet-demo",
      "region": "us-east-2",
      "flavor": "aws"
    },
    "storage-credential": {
      "type": "access-key",
      "aws-access-key-id": "YOUR_AWS_ACCESS_KEY_ID",
      "aws-secret-access-key": "YOUR_AWS_SECRET_ACCESS_KEY"
    }
  }'
```

### 4. Warehouse UUID

The UUID returned goes in `ha_parquet.cc`:

    fe89d40e-3472-11f1-8805-6fc69e665327

### 5. UI

    http://localhost:8181



