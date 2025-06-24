-- Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.
-- 
-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation; version 2 of the License.
-- 
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
-- 
-- You should have received a copy of the GNU General Public License
-- along with this program; if not, write to the Free Software
-- Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

DROP PROCEDURE IF EXISTS table_exists;

DELIMITER $$

CREATE DEFINER='mariadb.sys'@'localhost' PROCEDURE table_exists (
        IN in_db VARCHAR(64), IN in_table VARCHAR(64),
        OUT out_exists ENUM('', 'BASE TABLE', 'VIEW', 'TEMPORARY', 'SEQUENCE', 'SYSTEM VIEW', 'TEMPORARY SEQUENCE')
    )
    COMMENT '
             Description
             -----------

             Tests whether the table specified in in_db and in_table exists either as a regular
             table, or as a temporary table. The returned value corresponds to the table that
             will be used, so if there''s both a temporary and a permanent table with the given
             name, then ''TEMPORARY'' will be returned.

             Parameters
             -----------

             in_db (VARCHAR(64)):
               The database name to check for the existence of the table in.

             in_table (VARCHAR(64)):
               The name of the table to check the existence of.

             out_exists ENUM('''', ''BASE TABLE'', ''VIEW'', ''TEMPORARY'', ''SEQUENCE'', ''SYSTEM VIEW'', ''TEMPORARY SEQUENCE''):
               The return value: whether the table exists. The value is one of:
                 * ''''                    - the table does not exist neither as a base table, view, sequence nor temporary table/sequence.
                 * ''BASE TABLE''          - the table name exists as a permanent base table table.
                 * ''VIEW''                - the table name exists as a view.
                 * ''TEMPORARY''           - the table name exists as a temporary table.
                 * ''SEQUENCE''            - the table name exists as a sequence.
                 * ''SYSTEM VIEW''         - the table name exists as a system view.
                 * ''TEMPORARY SEQUENCE''  - the table name exists as a temporary sequence.

             Example
             --------

             MariaDB [sys]> CREATE DATABASE db1;
             Query OK, 1 row affected (0.07 sec)

             MariaDB [sys]> use db1;
             Database changed

             MariaDB [sys]> CREATE TABLE t1 (id INT PRIMARY KEY);
             Query OK, 0 rows affected (0.08 sec)

             MariaDB [sys]> CREATE TABLE t2 (id INT PRIMARY KEY);
             Query OK, 0 rows affected (0.08 sec)

             MariaDB [sys]> CREATE view v_t1 AS SELECT * FROM t1;
             Query OK, 0 rows affected (0.00 sec)

             MariaDB [sys]> CREATE TEMPORARY TABLE t1 (id INT PRIMARY KEY);
             Query OK, 0 rows affected (0.00 sec)

             MariaDB [sys]> CREATE SEQUENCE s;
             Query OK, 0 rows affected (0.00 sec)

             MariaDB [sys]> CREATE TEMPORARY SEQUENCE s_temp;
             Query OK, 0 rows affected (0.00 sec)

             MariaDB [sys]> CALL sys.table_exists(''db1'', ''t1'', @exists); SELECT @exists;
             Query OK, 0 rows affected (0.00 sec)

             +------------+
             | @exists    |
             +------------+
             | TEMPORARY  |
             +------------+
             1 row in set (0.00 sec)
             
             MariaDB [sys]> CALL sys.table_exists(''db1'', ''t2'', @exists); SELECT @exists;
             Query OK, 0 rows affected (0.00 sec)
             
             +------------+
             | @exists    |
             +------------+
             | BASE TABLE |
             +------------+
             1 row in set (0.01 sec)

             MariaDB [sys]> CALL sys.table_exists(''db1'', ''v_t1'', @exists); SELECT @exists;
             Query OK, 0 rows affected (0.00 sec)

             +---------+
             | @exists |
             +---------+
             | VIEW    |
             +---------+
             1 row in set (0.00 sec)

             MariaDB [sys]> CALL sys.table_exists(''db1'', ''s'', @exists); SELECT @exists;
             Query OK, 0 rows affected (0.006 sec)

             +----------+
             | @exists  |
             +----------+
             | SEQUENCE |
             +----------+
             1 row in set (0.000 sec)

             MariaDB [sys]> CALL table_exists(''information_schema'', ''user_variables'', @exists); SELECT @exists;
             Query OK, 0 rows affected (0.003 sec)

             +-------------+
             | @exists     |
             +-------------+
             | SYSTEM VIEW |
             +-------------+
             1 row in set (0.001 sec)

             MariaDB [sys]> CALL sys.table_exists(''db1'', ''t3'', @exists); SELECT @exists;
             Query OK, 0 rows affected (0.01 sec)

             +---------+
             | @exists |
             +---------+
             |         |
             +---------+
             1 row in set (0.00 sec)

             MariaDB [sys]> CALL table_exists(''db1'', ''s_temp'', @exists); SELECT @exists;
             Query OK, 0 rows affected (0.003 sec)

             +--------------------+
             | @exists            |
             +--------------------+
             | TEMPORARY SEQUENCE |
             +--------------------+
             1 row in set (0.001 sec)
            '
    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    CONTAINS SQL
BEGIN
    DECLARE v_error BOOLEAN DEFAULT FALSE;
    DECLARE db_quoted VARCHAR(64);
    DECLARE table_quoted VARCHAR(64);
    DECLARE v_table_type VARCHAR(30) DEFAULT '';
    DECLARE CONTINUE HANDLER FOR 1050 SET v_error = TRUE;
    DECLARE CONTINUE HANDLER FOR 1146 SET v_error = TRUE;

    -- First check do we have multiple rows, what can happen if temporary table
    -- and/or sequence is shadowing base table for example.
    -- In such scenario return temporary.
    SET v_table_type = (SELECT GROUP_CONCAT(TABLE_TYPE) FROM information_schema.TABLES WHERE
                            TABLE_SCHEMA = in_db AND TABLE_NAME = in_table);

    IF v_table_type LIKE '%,%' THEN
        SET out_exists = 'TEMPORARY';
    ELSE
        IF v_table_type is NULL
        THEN
            SET v_table_type='';
        END IF;
        -- Don't fail on table_type='SYSTEM VERSIONED'
        -- but return 'BASE TABLE' for compatibility with existing tooling
        IF v_table_type = 'SYSTEM VERSIONED' THEN
            SET out_exists = 'BASE TABLE';
        ELSE
            SET out_exists = v_table_type;
        END IF;
    END IF;
END$$

DELIMITER ;
