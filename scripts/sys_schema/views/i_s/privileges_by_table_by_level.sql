--
-- View: privileges_by_table_by_level
--
-- Shows granted privileges broken down by the table on which they allow access
-- and the level on which they were granted:
-- - user_privileges
-- - schema_privileges
-- - table_privileges
--
-- mysql> select * from sys.privileges_by_table_by_level;
-- +--------------+------------+--------------------+----------------+--------+
-- | TABLE_SCHEMA | TABLE_NAME | GRANTEE            | PRIVILEGE_TYPE | LEVEL  |
-- +--------------+------------+--------------------+----------------+--------+
-- | test         | v1         | 'oleg'@'localhost' | SELECT         | GLOBAL |
-- | test         | t1         | 'oleg'@'localhost' | SELECT         | GLOBAL |
-- | test         | v1         | 'oleg'@'localhost' | INSERT         | GLOBAL |
-- | test         | t1         | 'oleg'@'localhost' | INSERT         | GLOBAL |
-- | test         | v1         | 'oleg'@'localhost' | UPDATE         | GLOBAL |
-- | test         | v1         | 'PUBLIC'@''        | SELECT         | SCHEMA |
-- | test         | t1         | 'PUBLIC'@''        | SELECT         | SCHEMA |
-- | test         | v1         | 'PUBLIC'@''        | INSERT         | SCHEMA |
-- | test         | t1         | 'PUBLIC'@''        | INSERT         | SCHEMA |
-- | test         | v1         | 'PUBLIC'@''        | UPDATE         | SCHEMA |
-- | test         | t1         | 'PUBLIC'@''        | UPDATE         | SCHEMA |
-- | test         | v1         | 'PUBLIC'@''        | DELETE HISTORY | SCHEMA |
-- | test         | t1         | 'PUBLIC'@''        | DELETE HISTORY | SCHEMA |
-- | test         | t1         | 'oleg'@'%'         | SELECT         | TABLE  |
-- | test         | t1         | 'oleg'@'%'         | UPDATE         | TABLE  |
-- | test         | v1         | 'oleg'@'%'         | SELECT         | TABLE  |
-- +--------------+------------+--------------------+----------------+--------+

CREATE OR REPLACE
  ALGORITHM = TEMPTABLE
  DEFINER = 'mariadb.sys'@'localhost'
  SQL SECURITY INVOKER
VIEW privileges_by_table_by_level (
  TABLE_SCHEMA,
  TABLE_NAME,
  GRANTEE,
  PRIVILEGE,
  LEVEL
) AS
SELECT t.TABLE_SCHEMA,
       t.TABLE_NAME,
       privs.GRANTEE,
       privs.PRIVILEGE_TYPE,
       privs.LEVEL
FROM INFORMATION_SCHEMA.TABLES AS t
JOIN ( SELECT NULL AS TABLE_SCHEMA,
              NULL AS TABLE_NAME,
              GRANTEE,
              PRIVILEGE_TYPE,
             'GLOBAL' LEVEL
           FROM INFORMATION_SCHEMA.USER_PRIVILEGES
         UNION
       SELECT TABLE_SCHEMA,
              NULL AS TABLE_NAME,
              GRANTEE,
              PRIVILEGE_TYPE,
              'SCHEMA' LEVEL
           FROM INFORMATION_SCHEMA.SCHEMA_PRIVILEGES
         UNION
       SELECT TABLE_SCHEMA,
              TABLE_NAME,
              GRANTEE,
              PRIVILEGE_TYPE,
              'TABLE' LEVEL
           FROM INFORMATION_SCHEMA.TABLE_PRIVILEGES
       ) privs
    ON (t.TABLE_SCHEMA = privs.TABLE_SCHEMA OR privs.TABLE_SCHEMA IS NULL)
   AND (t.TABLE_NAME = privs.TABLE_NAME OR privs.TABLE_NAME IS NULL)
   AND privs.PRIVILEGE_TYPE IN ('SELECT', 'INSERT', 'UPDATE', 'DELETE',
                                'CREATE', 'ALTER', 'DROP', 'INDEX',
                                'REFERENCES', 'TRIGGER', 'GRANT OPTION',
                                'SHOW VIEW', 'DELETE HISTORY')
WHERE t.TABLE_SCHEMA NOT IN ('sys', 'mysql','information_schema',
                             'performance_schema');
