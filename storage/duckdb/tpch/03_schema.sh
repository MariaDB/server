#!/usr/bin/env bash
# Create database $SCHEMA and the 8 TPC-H tables as ENGINE=DUCKDB, directly
# through the mariadb client. DROP + CREATE makes this idempotent.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$DIR/config.sh"

echo "Creating database '$SCHEMA' and ENGINE=DUCKDB tables ..."
mdb "CREATE DATABASE IF NOT EXISTS $SCHEMA"

for t in "${TABLES[@]}"; do
  mdb_db "DROP TABLE IF EXISTS $t"
done

mdb_db "CREATE TABLE region   (r_regionkey INTEGER PRIMARY KEY, r_name VARCHAR(25), r_comment VARCHAR(152)) ENGINE=DUCKDB DEFAULT CHARSET=$CHARSET"
mdb_db "CREATE TABLE nation   (n_nationkey INTEGER PRIMARY KEY, n_name VARCHAR(25), n_regionkey INTEGER, n_comment VARCHAR(152)) ENGINE=DUCKDB DEFAULT CHARSET=$CHARSET"
mdb_db "CREATE TABLE supplier (s_suppkey INTEGER PRIMARY KEY, s_name VARCHAR(25), s_address VARCHAR(40), s_nationkey INTEGER, s_phone VARCHAR(15), s_acctbal DECIMAL(15,2), s_comment VARCHAR(101)) ENGINE=DUCKDB DEFAULT CHARSET=$CHARSET"
mdb_db "CREATE TABLE customer (c_custkey INTEGER PRIMARY KEY, c_name VARCHAR(25), c_address VARCHAR(40), c_nationkey INTEGER, c_phone VARCHAR(15), c_acctbal DECIMAL(15,2), c_mktsegment VARCHAR(10), c_comment VARCHAR(117)) ENGINE=DUCKDB DEFAULT CHARSET=$CHARSET"
mdb_db "CREATE TABLE part     (p_partkey INTEGER PRIMARY KEY, p_name VARCHAR(55), p_mfgr VARCHAR(25), p_brand VARCHAR(10), p_type VARCHAR(25), p_size INTEGER, p_container VARCHAR(10), p_retailprice DECIMAL(15,2), p_comment VARCHAR(23)) ENGINE=DUCKDB DEFAULT CHARSET=$CHARSET"
mdb_db "CREATE TABLE partsupp (ps_partkey INTEGER, ps_suppkey INTEGER, ps_availqty INTEGER, ps_supplycost DECIMAL(15,2), ps_comment VARCHAR(199), PRIMARY KEY (ps_partkey, ps_suppkey)) ENGINE=DUCKDB DEFAULT CHARSET=$CHARSET"
mdb_db "CREATE TABLE orders   (o_orderkey BIGINT PRIMARY KEY, o_custkey INTEGER, o_orderstatus CHAR(1), o_totalprice DECIMAL(15,2), o_orderdate DATE, o_orderpriority VARCHAR(15), o_clerk VARCHAR(15), o_shippriority INTEGER, o_comment VARCHAR(79)) ENGINE=DUCKDB DEFAULT CHARSET=$CHARSET"
mdb_db "CREATE TABLE lineitem (l_orderkey BIGINT, l_partkey INTEGER, l_suppkey INTEGER, l_linenumber INTEGER, l_quantity DECIMAL(15,2), l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2), l_tax DECIMAL(15,2), l_returnflag CHAR(1), l_linestatus CHAR(1), l_shipdate DATE, l_commitdate DATE, l_receiptdate DATE, l_shipinstruct VARCHAR(25), l_shipmode VARCHAR(10), l_comment VARCHAR(44), PRIMARY KEY (l_orderkey, l_linenumber)) ENGINE=DUCKDB DEFAULT CHARSET=$CHARSET"
echo "Database '$SCHEMA' ready."
