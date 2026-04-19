Add the access keys: Check groupchat for keys
Replace placeholders in TWO different spots:
con->Query("SET s3_access_key_id='YOUR_AWS_ACCESS_KEY_ID'");
con->Query("SET s3_secret_access_key='YOUR_AWS_SECRET_ACCESS_KEY'");

1. Build MariaDB with DuckDB:

cd ~/MariaDB2/build-mariadb-duckdb
cmake ../server \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWITH_UNIT_TESTS=OFF \
  -DBISON_EXECUTABLE="$(brew --prefix bison)/bin/bison"
cmake --build . --parallel 4

2. Initialize the database:

sql/mariadb-install-db \
  --srcdir=../server \
  --defaults-file=~/mariadb-parquet.cnf

3. Start the server:

sql/mariadbd --defaults-file=~/mariadb-parquet.cnf &

4. Connect:

client/mariadb --socket=/tmp/mariadb-parquet.sock


This cmakelists.txt worked for me: 
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


Lakekeeper Setup:
1. Clone and start LakeKeeper:

bashgit clone https://github.com/lakekeeper/lakekeeper
cd lakekeeper/examples/minimal
docker compose up -d

2. Verify it's running:

bashcurl http://localhost:8181/health

3. Create the warehouse (one time):

bashcurl -X POST http://localhost:8181/management/v1/warehouse \
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

4. The warehouse UUID it returns is what goes in ha_parquet.cc:
fe89d40e-3472-11f1-8805-6fc69e665327

5. UI is at:
http://localhost:8181
