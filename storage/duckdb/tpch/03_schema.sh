#!/usr/bin/env bash
# Create schema $SCHEMA and the 8 TPC-H tables inside the embedded DuckDB,
# via run_in_duckdb. CREATE OR REPLACE makes this idempotent.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$DIR/config.sh"

echo "Creating schema '$SCHEMA' and tables ..."
duck "CREATE SCHEMA IF NOT EXISTS $SCHEMA" >/dev/null
duck "CREATE OR REPLACE TABLE $SCHEMA.region   (r_regionkey INTEGER PRIMARY KEY, r_name VARCHAR, r_comment VARCHAR)" >/dev/null
duck "CREATE OR REPLACE TABLE $SCHEMA.nation   (n_nationkey INTEGER PRIMARY KEY, n_name VARCHAR, n_regionkey INTEGER, n_comment VARCHAR)" >/dev/null
duck "CREATE OR REPLACE TABLE $SCHEMA.supplier (s_suppkey INTEGER PRIMARY KEY, s_name VARCHAR, s_address VARCHAR, s_nationkey INTEGER, s_phone VARCHAR, s_acctbal DECIMAL(15,2), s_comment VARCHAR)" >/dev/null
duck "CREATE OR REPLACE TABLE $SCHEMA.customer (c_custkey INTEGER PRIMARY KEY, c_name VARCHAR, c_address VARCHAR, c_nationkey INTEGER, c_phone VARCHAR, c_acctbal DECIMAL(15,2), c_mktsegment VARCHAR, c_comment VARCHAR)" >/dev/null
duck "CREATE OR REPLACE TABLE $SCHEMA.part     (p_partkey INTEGER PRIMARY KEY, p_name VARCHAR, p_mfgr VARCHAR, p_brand VARCHAR, p_type VARCHAR, p_size INTEGER, p_container VARCHAR, p_retailprice DECIMAL(15,2), p_comment VARCHAR)" >/dev/null
duck "CREATE OR REPLACE TABLE $SCHEMA.partsupp (ps_partkey INTEGER, ps_suppkey INTEGER, ps_availqty INTEGER, ps_supplycost DECIMAL(15,2), ps_comment VARCHAR, PRIMARY KEY (ps_partkey, ps_suppkey))" >/dev/null
duck "CREATE OR REPLACE TABLE $SCHEMA.orders   (o_orderkey BIGINT PRIMARY KEY, o_custkey INTEGER, o_orderstatus VARCHAR, o_totalprice DECIMAL(15,2), o_orderdate DATE, o_orderpriority VARCHAR, o_clerk VARCHAR, o_shippriority INTEGER, o_comment VARCHAR)" >/dev/null
duck "CREATE OR REPLACE TABLE $SCHEMA.lineitem (l_orderkey BIGINT, l_partkey INTEGER, l_suppkey INTEGER, l_linenumber INTEGER, l_quantity DECIMAL(15,2), l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2), l_tax DECIMAL(15,2), l_returnflag VARCHAR, l_linestatus VARCHAR, l_shipdate DATE, l_commitdate DATE, l_receiptdate DATE, l_shipinstruct VARCHAR, l_shipmode VARCHAR, l_comment VARCHAR, PRIMARY KEY (l_orderkey, l_linenumber))" >/dev/null
echo "Schema '$SCHEMA' ready."
