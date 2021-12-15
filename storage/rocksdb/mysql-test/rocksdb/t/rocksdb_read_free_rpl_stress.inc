
disable_query_log;

# Create a schema with different kinds of tables (with different kinds of keys)
connection master;
create table t1(a int, b int, c int, d int); #no keys
create table t2(a int primary key, b int, c int, d int); #only pk
create table t3(a int, b int, c int, d int, key(b)); #only sk
create table t4(a int, b int unique, c int, d int); #only unique sk
create table t5(a int primary key, b int, c int, d int, key(b)); #pk + sk
create table t6(a int primary key, b int unique, c int, d int); #pk + unique sk
create table t7(a int, b int unique, c int, d int, key(c)); #sk + unique sk
create table t8(a int primary key, b int unique, c int, d int, key(c)); #pk + sk + unique sk

# Insert a bunch of rows
let $iter = 0;
while ($iter < 1000) {
  let $t = 1;
  while ($t <= 8) {
    eval insert into t$t values($iter, $iter, $iter, $iter);
    inc $t;
  }
  inc $iter;
}

let $iter = 0;
while ($iter < 10) {
  let $t = 1;
  while ($t <= 8) {
    eval update t$t set a = a + 10000 where a > 900; # update pk (if any)
    eval update t$t set b = b + 10000 where b > 900; # update sk or unique (if any)
    eval update t$t set c = c + 10000 where c > 900; # update sk or unique(if any)
    eval update t$t set d = d + 10000 where d > 900; # update non key col

    eval delete from t$t where a < 25;
    eval delete from t$t where b < 50;
    eval delete from t$t where c < 75;
    eval delete from t$t where d < 100;

    # Re-insert the deleted rows
    let $i = 0;
    while ($i < 100) {
      eval insert into t$t values($i, $i, $i, $i);
      inc $i;
    }
    inc $t;
  }
  inc $iter;
}
source include/sync_slave_sql_with_master.inc;

connection master;
let $t = 1;
while ($t <= 8) {
  let $diff_tables = master:t$t, slave:t$t;
  source include/diff_tables.inc;
  inc $t;
}

# Cleanup
connection master;
let $t = 1;
while ($t <= 8) {
  eval drop table t$t;
  inc $t;
}
source include/sync_slave_sql_with_master.inc;

enable_query_log;
