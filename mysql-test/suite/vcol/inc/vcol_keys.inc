################################################################################
# inc/vcol_keys.inc                                                            #
#                                                                              #
# Purpose:                                                                     #
#  Testing keys, indexes defined upon virtual columns.                         #
#                                                                              #
#                                                                              #
#                                                                              #
#------------------------------------------------------------------------------#
# Original Author: Andrey Zhakov                                               #
# Original Date: 2008-09-02                                                    #
# Change Author: Oleksandr Byelkin (Monty program Ab)
# Date: 2009-03-24 
# Change: Syntax changed
################################################################################


--echo #            - UNIQUE KEY
--echo #            - INDEX
--echo #            - FULLTEXT INDEX
--echo #            - SPATIAL INDEX (not supported)
--echo #            - FOREIGN INDEX (partially supported)
--echo #            - CHECK (allowed but not used)

--echo # UNIQUE
create table t1 (a int, b int as (a*2) unique);
drop table t1;
create table t1 (a int, b int as (a*2) persistent unique);
show create table t1;
describe t1;
drop table t1;

create table t1 (a int, b int as (a*2), unique key (b));
drop table t1;
create table t1 (a int, b int as (a*2) persistent, unique (b));
show create table t1;
describe t1;
drop table t1;

create table t1 (a int, b int as (a*2));
alter table t1 add unique key (b);
drop table t1;
create table t1 (a int, b int as (a*2) persistent);
alter table t1 add unique key (b);
drop table t1;

--echo # Testing data manipulation operations involving UNIQUE keys 
--echo # on virtual columns can be found in:
--echo #  - vcol_ins_upd.inc
--echo #  - vcol_select.inc

--echo # 
--echo # INDEX
create table t1 (a int, b int as (a*2), index (b));
drop table t1;
create table t1 (a int, b int as (a*2), index (a,b));
drop table t1;

create table t1 (a int, b int as (a*2) persistent, index (b));
show create table t1;
describe t1;
drop table t1;

create table t1 (a int, b int as (a*2) persistent, index (a,b));
show create table t1;
describe t1;
drop table t1;

create table t1 (a int, b int as (a*2));
alter table t1 add index (b);
alter table t1 add index (a,b);
drop table t1;

create table t1 (a int, b int as (a*2) persistent);
alter table t1 add index (b);
drop table t1;

create table t1 (a int, b int as (a*2) persistent);
alter table t1 add index (a,b);
create table t2 like t1;
drop table t2;
drop table t1;

--echo # Testing data manipulation operations involving INDEX
--echo # on virtual columns can be found in:
--echo #  - vcol_select.inc

--echo #
--echo # TODO: FULLTEXT INDEX

--echo # SPATIAL INDEX
if (!$skip_spatial_index_check)
{
  --echo # Error "All parts of a SPATIAL index must be NOT NULL"
  --error ER_SPATIAL_CANT_HAVE_NULL
  create table t1 (a int, b geometry as (a+1) persistent, spatial index (b));
  create table t1 (a int, b int as (a+1) persistent);
  --error ER_WRONG_ARGUMENTS
  alter table t1 add spatial index (b);
  drop table t1;
}

--echo # FOREIGN KEY

--echo # Rejected FK options.
--error ER_WRONG_FK_OPTION_FOR_GENERATED_COLUMN 
create table t1 (a int, b int as (a+1) persistent,
                 foreign key (b) references t2(a) on update set null);
--error ER_WRONG_FK_OPTION_FOR_GENERATED_COLUMN 
create table t1 (a int, b int as (a+1) persistent,
                 foreign key (b) references t2(a) on update cascade);
--error ER_WRONG_FK_OPTION_FOR_GENERATED_COLUMN 
create table t1 (a int, b int as (a+1) persistent,
                 foreign key (b) references t2(a) on delete set null);

create table t1 (a int, b int as (a+1) persistent);
--error ER_WRONG_FK_OPTION_FOR_GENERATED_COLUMN 
alter table t1 add foreign key (b) references t2(a) on update set null;
--error ER_WRONG_FK_OPTION_FOR_GENERATED_COLUMN 
alter table t1 add foreign key (b) references t2(a) on update cascade;
--error ER_WRONG_FK_OPTION_FOR_GENERATED_COLUMN 
alter table t1 add foreign key (b) references t2(a) on delete set null;
drop table t1;

if ($with_foreign_keys) {
--error ER_CANT_CREATE_TABLE
create table t1 (a int, b int as (a+1), foreign key (b) references t2(a));

create table t1 (a int, b int as (a+1));
--error ER_CANT_CREATE_TABLE
alter table t1 add foreign key (b) references t2(a);
drop table t1;
}

--echo # Allowed FK options.
create table t2 (a int primary key, b char(5));
create table t1 (a int, b int as (a % 10) persistent,
                 foreign key (b) references t2(a) on update restrict);
drop table t1;
create table t1 (a int, b int as (a % 10) persistent,
                 foreign key (b) references t2(a) on update no action);
drop table t1;
create table t1 (a int, b int as (a % 10) persistent,
                 foreign key (b) references t2(a) on delete restrict);
drop table t1;
create table t1 (a int, b int as (a % 10) persistent,
                 foreign key (b) references t2(a) on delete cascade);
drop table t1;
create table t1 (a int, b int as (a % 10) persistent,
                 foreign key (b) references t2(a) on delete no action);
drop table t1;

--echo 
--echo # Testing data manipulation operations involving FOREIGN KEY 
--echo # on virtual columns can be found in:
--echo #  - vcol_ins_upd.inc
--echo #  - vcol_select.inc

#
# Restrictions when indexed:
#

--error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b timestamp as (now()), key (b));
create table t1 (a int, b timestamp as (now()));
--error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
alter table t1 add index (b);
drop table t1;

--error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b varchar(100) as (user()), key (b));
create table t1 (a int, b varchar(100) as (user()));
--error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
alter table t1 add index (b);
drop table t1;

--error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
create table t1 (a int, b double as (rand()), key (b));
create table t1 (a int, b double as (rand()));
--error ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED
alter table t1 add index (b);
drop table t1;

#
# MDEV-11598 Assertion `!table || (!table->read_set... failed
#

CREATE OR REPLACE TABLE t1 (
    f2 DOUBLE NOT NULL DEFAULT '0',
    f3 DOUBLE NOT NULL DEFAULT '0',
    f4 DOUBLE,
    f5 DOUBLE DEFAULT '0',
    v4 DOUBLE AS (IF(f4,f3,f2)) VIRTUAL,
    KEY (f5),
    KEY (v4)
);

INSERT INTO t1 (f2,f3,f4,f5) VALUES (5,4,1,0),(5,7,NULL,0);
INSERT INTO t1 (f2,f3,f4,f5) SELECT f2, f3, f5, f3 FROM t1;
INSERT INTO t1 (f2,f3,f4,f5) VALUES (5,0,NULL,1);
INSERT INTO t1 (f2,f3,f4,f5) SELECT f2, f5, f5, f3 FROM t1;
DELETE FROM t1 WHERE f5 = 1 OR v4 = 4 ORDER BY f5,v4 LIMIT 9;
SELECT * from t1;
DROP TABLE t1;

# Another similar failure

CREATE TABLE t1 (
 d DECIMAL(63,0) NOT NULL DEFAULT 0,
 c VARCHAR(64) NOT NULL DEFAULT '',
 vd DECIMAL(63,0) AS (d) VIRTUAL,
 vc VARCHAR(2048) AS (c) VIRTUAL,
 pk BIGINT AUTO_INCREMENT,
 PRIMARY KEY(pk));

INSERT INTO t1 (d,c) VALUES (0.5,'foo');
SELECT * FROM t1 WHERE vc != 'bar' ORDER BY vd;
DROP TABLE t1;

#
# MDEV-11729:  Crash when using partial indexed virtual fields
#

CREATE TABLE t1 (
 pk BIGINT,
 c CHAR(64) NOT NULL DEFAULT '',
 vc CHAR(64) AS (c) VIRTUAL,
 PRIMARY KEY(pk),
 INDEX(vc(32))
);
DELETE FROM t1 WHERE vc IS NULL ORDER BY pk;
DROP TABLE t1;

