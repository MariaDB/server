set optimizer_record_context=OFF;
set optimizer_trace=1;

create database db1;
use db1;

--source include/round_cost_function.inc

let $table_name=t1;

--echo #
--echo # Range query on a single table by using 1 unique index on a single column
--echo #

create table t1 (
    c1 int,
    c2 int,
    unique(c1),
    unique(c2)
) Engine=InnoDB;

insert into t1 select seq, seq from seq_1_to_100;

analyze table t1;

let $explain_query= explain format= json select * from t1 a,
    t1 b where a.c1 < 3 and b.c1 < 33;

--source include/opt_ctx_cmp_2_runs_of_query.inc

drop table t1;

--echo #
--echo # Equi-Join query on a single table having 1 non-unique index on a single column.
--echo # Also, index column is used in the condition
--echo #

create table t1 (
    c1 int,
    c2 int,
    index(c1)
) Engine=InnoDB;

insert into t1 select seq%5, seq%10 from seq_1_to_200;

analyze table t1;

let $explain_query=explain format=json select * from t1 as tt1, t1 as tt2 where tt1.c1 = tt2.c1;

--source include/opt_ctx_cmp_2_runs_of_query.inc

drop table t1;

--echo #
--echo # Equi-Join query on a single table having 1 primary key index on a single column.
--echo # Also, index column is used in the condition
--echo #

create table t1 (
    c1 int,
    c2 int,
    primary key (c1)
) Engine=InnoDB;

insert into t1 select seq, seq%5 from seq_1_to_100;

analyze table t1;

let $explain_query=explain format=json select * from t1 as tt1, t1 as tt2 where tt1.c1 = tt2.c1;

--source include/opt_ctx_cmp_2_runs_of_query.inc

drop table t1;

--echo #
--echo # Equi-Join query on a single table having 1 primary key index on 2 columns.
--echo # Both the index columns are used in the condition
--echo #

let $table_name=t1;

create table t1 (
    c1 int,
    c2 int,
    primary key (c1, c2)
) Engine=InnoDB;

insert into t1 select seq, seq%5 from seq_1_to_100;

analyze table t1;

let $explain_query=explain format=json select * from t1 as tt1, t1 as tt2 where tt1.c1 = tt2.c1 and tt1.c2 = tt2.c2;

--source include/opt_ctx_cmp_2_runs_of_query.inc

drop table t1;

--echo #
--echo # Equi-Join query on a single table having 1 non-unique index on 2 columns.
--echo # However, only 1 column from the index is used in the condition
--echo #

create table t1 (
    c1 int,
    c2 int,
    index(c1, c2)
) Engine=InnoDB;

insert into t1 select seq%5, seq%10 from seq_1_to_100;

analyze table t1;

let $explain_query=explain format=json select * from t1 as tt1, t1 as tt2 where tt1.c1 = tt2.c1;

--source include/opt_ctx_cmp_2_runs_of_query.inc

drop table t1;

--echo #
--echo # Equi-Join query on a single table having 1 primary key index on 2 columns.
--echo # However, only 1 column from the index that has unique values is used in the condition
--echo #

create table t1 (
    c1 int,
    c2 int,
    primary key (c1, c2)
) Engine=InnoDB;

insert into t1 select seq, seq%5 from seq_1_to_100;

analyze table t1;

let $explain_query=explain format=json select * from t1 as tt1, t1 as tt2 where tt1.c1 = tt2.c1;

--source include/opt_ctx_cmp_2_runs_of_query.inc

--echo #
--echo # Equi-Join query on a single table having 1 primary key index on 2 columns.
--echo # However, only 1 column from the index that has non-unique values is used in the condition
--echo #

insert into t1 select seq, seq%5 from seq_1_to_100;

analyze table t1;

let $explain_query=explain format=json select * from t1 as tt1, t1 as tt2 where tt1.c2 = tt2.c2;

--source include/opt_ctx_cmp_2_runs_of_query.inc

--echo #
--echo # Query on a single table having 1 primary key index on 2 columns.
--echo # However, a constant literal is used in the equality predicate with only 1 column from the index that has unique values.
--echo #

insert into t1 select seq, seq%5 from seq_1_to_100;

analyze table t1;

let $explain_query=explain format=json select * from t1 as tt1 where tt1.c1 = 5;

--source include/opt_ctx_cmp_2_runs_of_query.inc

--echo #
--echo # Query on a single table having 1 primary key index on 2 columns.
--echo # However, a constant literal is used in the equality predicate with only 1 column from the index that has non-unique values.
--echo #

insert into t1 select seq, seq%5 from seq_1_to_100;

analyze table t1;

let $explain_query=explain format=json select * from t1 as tt1 where tt1.c2 = 5;

--source include/opt_ctx_cmp_2_runs_of_query.inc

drop table t1;

--echo #
--echo # Query on a single table having 1 non-unique index on 2 columns.
--echo # However, a constant literal is used in the equality predicate using only 1 column from the index.
--echo #

create table t1 (
    c1 int,
    c2 int,
    index(c1)
) Engine=InnoDB;

insert into t1 select seq%3, seq%5 from seq_1_to_100;

analyze table t1;

let $explain_query=explain format=json select * from t1 as tt1 where tt1.c1 = 3;

--source include/opt_ctx_cmp_2_runs_of_query.inc

--echo #
--echo # Query on a single table having 1 non-unique index on a single column.
--echo # Also, a constant literal is used in the equality predicate on the column that is in the index.
--echo #

insert into t1 select seq%3, seq%5 from seq_1_to_100;

analyze table t1;

let $explain_query=explain format=json select * from t1 as tt1 where tt1.c1 = 5;

--source include/opt_ctx_cmp_2_runs_of_query.inc

drop table t1;

--echo #
--echo # Query on a single table having 1 primary key index with only 1 column.
--echo # However, a constant literal is used in the equality predicate on the column that is not in the index.
--echo #

create table t1 (
    c1 int,
    c2 int,
    primary key(c1)
) Engine=InnoDB;

insert into t1 select seq, seq%5 from seq_1_to_100;

analyze table t1;

let $explain_query=explain format=json select * from t1 as tt1 where tt1.c2 = 4;

--source include/opt_ctx_cmp_2_runs_of_query.inc

drop table t1;

--echo #
--echo # Index-Merge query on a single table having 2 non-unique index with a single column in each.
--echo # Also, index column is used in the condition
--echo #

create table t1 (
    c1 int,
    c2 int,
    index(c1),
    index(c2)
) Engine=InnoDB;

insert into t1 select seq%5, seq%10 from seq_1_to_100;

analyze table t1;

let $explain_query=explain format=json select * from t1 as tt1 where tt1.c1 = 5 OR tt1.c2 = 10;

--source include/opt_ctx_cmp_2_runs_of_query.inc

drop table t1;

--echo #
--echo # Index-Merge query on a single table having 2 indexes with overlapping keys
--echo #

create table t1 (
    a int,
    b int,
    c int,
    index idx_ab(a, b),
    index idx_ac(a, c)
) Engine=InnoDB;

insert into t1 select seq%2, seq%3, seq%5 from seq_1_to_20;

analyze table t1;

let $explain_query=explain format=json select * from t1 where a=1 and b=1 and c=1;

--source include/opt_ctx_cmp_2_runs_of_query.inc

drop table t1;

drop function round_cost;

drop database db1;
