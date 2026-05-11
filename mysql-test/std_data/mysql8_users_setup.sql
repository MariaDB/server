-- =====================================================================
-- Test fixture: MySQL 8.x state for round-trip testing of mariadb-dump.
--
-- Workflow:
--   1. mysql -uroot -p < mysql8_users_setup.sql   (on a fresh MySQL 8.x)
--   2. ./client/mariadb-dump --plugin-dir=$BUILD/libmariadb \
--        -h <host> -P <port> -u root --system=users > mysql8_users_dump.sql
--   3. Commit the captured dump.
--
-- Step 1 needs a SUPER user (or SYSTEM_VARIABLES_ADMIN): Section 18 sets
-- partial_revokes. The committed capture was taken as root; m8_dumper of
-- Section 2 has the privileges a non-root dump needs.
--
-- Names describe the case they exercise (m8_dyn_mapped, ...) so a failing
-- import test points at it. Re-runs are safe (drops everything first).
-- COMMENT/ATTRIBUTE need MySQL 8.0.21+, FAILED_LOGIN_ATTEMPTS 8.0.19+.
-- =====================================================================

-- Section 0: cleanup
SET GLOBAL mandatory_roles = '';
DELETE FROM mysql.role_edges WHERE TO_USER = 'm8_orphan_member';
DROP USER IF EXISTS
  'm8_dumper'@'%',
  'm8_simple'@'%',
  'm8_static_priv'@'%',
  'm8_dyn_mapped'@'%',
  'm8_dyn_unmapped'@'%',
  'm8_dyn_mixed'@'%',
  'm8_dyn_all_dropped'@'%',
  'm8_static_dyn_mix'@'%',
  'm8_role_static'@'%',
  'm8_static_role_mix'@'%',
  'm8_dyn_grant_opt'@'%',
  'm8_dyn_flush_collapse'@'%',
  'm8_col_priv'@'%',
  'm8_db_underscore'@'%',
  'm8 edge space'@'%',
  'm8_edge@in_name'@'%',
  'm8_edge_wildcard_host'@'192.168.%.%',
  'm8_all_privs'@'%',
  'm8_multiscope'@'%',
  'm8_proc_exec'@'%',
  'm8_func_exec'@'%',
  'm8_routine_admin'@'%',
  'm8_proxy_target'@'%',
  'm8_proxy_user'@'%',
  'm8_usage_only'@'%',
  'm8_lock'@'%',
  'm8_unlock'@'%',
  'm8_default_role_one'@'%',
  'm8_default_role_many'@'%',
  'm8_pwd_history'@'%',
  'm8_pwd_reuse'@'%',
  'm8_pwd_require_current'@'%',
  'm8_pwd_expire_now'@'%',
  'm8_pwd_expire_never'@'%',
  'm8_pwd_expire_interval'@'%',
  'm8_pwd_expire_default'@'%',
  'm8_max_conn'@'%',
  'm8_resource_full'@'%',
  'm8_require_ssl'@'%',
  'm8_require_x509'@'%',
  'm8_require_cipher'@'%',
  'm8_failed_login'@'%',
  'm8_with_comment'@'%',
  'm8_with_attribute'@'%',
  'm8_combined'@'%',
  'm8_shared_user_1'@'%',
  'm8_shared_user_2'@'%',
  'm8_shared_user_3'@'%',
  'm8_shared_user_4'@'%',
  'm8_shared_user_5'@'%',
  'm8_auth_sha256'@'%',
  'm8_auth_no_pwd'@'%',
  'm8_auth_caching_sha2'@'%',
  'm8_host_localhost'@'localhost',
  'm8_host_dual'@'localhost',
  'm8_host_dual'@'%',
  ''@'localhost',
  ''@'%',
  'O\'Brien'@'%',
  'm8_用户'@'%',
  'm8_starslash_comment'@'%',
  'm8_starslash_escaped'@'%',
  'm8_partial_revoke'@'%',
  'm8_system_grants'@'%',
  'm8_hybrid'@'%',
  'm8_hybrid_member'@'%',
  'm8_dual_member'@'%';

DROP ROLE IF EXISTS
  'm8_role_a','m8_role_b','m8_role_c','m8_role_d',
  'm8_default_a','m8_default_b',
  'm8_shared_role','m8_admin_chain_root',
  `m8_role*/starslash`,
  'm8_role_dual'@'h1','m8_role_dual'@'h2';

DROP DATABASE IF EXISTS m8_db;
DROP DATABASE IF EXISTS `m8_under_score_db`;

-- Section 1: schemas, tables, routines
CREATE DATABASE m8_db;
CREATE TABLE m8_db.tab (col_a INT, col_underscore_b INT, plain INT);
CREATE TABLE m8_db.tab2 (id INT PRIMARY KEY, payload TEXT);
CREATE VIEW m8_db.v AS SELECT col_a FROM m8_db.tab;

DELIMITER $$
CREATE PROCEDURE m8_db.proc_one(IN x INT) BEGIN SELECT x; END$$
CREATE FUNCTION  m8_db.func_one(x INT) RETURNS INT DETERMINISTIC
  BEGIN RETURN x * 2; END$$
DELIMITER ;

CREATE DATABASE `m8_under_score_db`;
CREATE TABLE `m8_under_score_db`.t (a INT);

-- Section 2: dumping user
CREATE USER 'm8_dumper'@'%' IDENTIFIED BY 'dumper_pw';
GRANT SELECT, LOCK TABLES, SHOW VIEW, EVENT, TRIGGER,
      RELOAD, PROCESS, REPLICATION CLIENT, SHOW DATABASES, CREATE USER
  ON *.* TO 'm8_dumper'@'%';
GRANT SELECT ON mysql.* TO 'm8_dumper'@'%';

-- Section 3: roles — 3-level admin chain + a 5-user-shared role
CREATE ROLE 'm8_role_a','m8_role_b','m8_role_c','m8_role_d';
CREATE ROLE 'm8_default_a','m8_default_b';
CREATE ROLE 'm8_shared_role','m8_admin_chain_root';

GRANT 'm8_role_b' TO 'm8_role_a' WITH ADMIN OPTION;
GRANT 'm8_role_c' TO 'm8_role_b' WITH ADMIN OPTION;
GRANT SELECT         ON m8_db.* TO 'm8_role_a';
GRANT INSERT         ON m8_db.* TO 'm8_role_b';
GRANT UPDATE         ON m8_db.* TO 'm8_role_c';
GRANT SHOW DATABASES ON *.*    TO 'm8_role_d';
GRANT 'm8_role_a' TO 'm8_admin_chain_root' WITH ADMIN OPTION;
GRANT SELECT  ON m8_db.tab               TO 'm8_shared_role';
GRANT EXECUTE ON PROCEDURE m8_db.proc_one TO 'm8_shared_role';

-- Orphan role with a literal '*/' in its name; its CREATE ROLE is emitted
-- split over string literals. Its BACKUP_ADMIN grant is unmapped, so it is
-- dropped with a # WARNING.
CREATE ROLE `m8_role*/starslash`;
GRANT BACKUP_ADMIN ON *.* TO `m8_role*/starslash`;

-- Section 4: GRANT translator coverage

-- 4.1 plain SELECT only -> translator NOT triggered
CREATE USER 'm8_simple'@'%' IDENTIFIED BY 'pw';
GRANT SELECT ON m8_db.* TO 'm8_simple'@'%';

-- 4.2 only static privileges across multiple grants
CREATE USER 'm8_static_priv'@'%' IDENTIFIED BY 'pw';
GRANT SELECT, INSERT, UPDATE, DELETE ON m8_db.* TO 'm8_static_priv'@'%';
GRANT RELOAD, PROCESS                ON *.*    TO 'm8_static_priv'@'%';

-- 4.3 single dynamic priv WITH MariaDB equivalent
CREATE USER 'm8_dyn_mapped'@'%' IDENTIFIED BY 'pw';
GRANT BINLOG_ADMIN ON *.* TO 'm8_dyn_mapped'@'%';

-- 4.4 single dynamic priv with NO MariaDB equivalent -> dropped + warn
CREATE USER 'm8_dyn_unmapped'@'%' IDENTIFIED BY 'pw';
GRANT BACKUP_ADMIN ON *.* TO 'm8_dyn_unmapped'@'%';

-- 4.5 mixed mapped + unmapped dynamic privs
CREATE USER 'm8_dyn_mixed'@'%' IDENTIFIED BY 'pw';
GRANT BACKUP_ADMIN, BINLOG_ADMIN, CONNECTION_ADMIN, AUDIT_ADMIN,
      REPLICATION_APPLIER, SHOW_ROUTINE
  ON *.* TO 'm8_dyn_mixed'@'%';

-- 4.6 all-unmapped -> entire MariaDB GRANT line suppressed
CREATE USER 'm8_dyn_all_dropped'@'%' IDENTIFIED BY 'pw';
GRANT BACKUP_ADMIN, AUDIT_ADMIN, CLONE_ADMIN ON *.* TO 'm8_dyn_all_dropped'@'%';

-- 4.7 static + dynamic in one GRANT
CREATE USER 'm8_static_dyn_mix'@'%' IDENTIFIED BY 'pw';
GRANT SELECT, BINLOG_ADMIN, BACKUP_ADMIN ON *.* TO 'm8_static_dyn_mix'@'%';

-- 4.8 MySQL-only role privs (CREATE ROLE / DROP ROLE)
CREATE USER 'm8_role_static'@'%' IDENTIFIED BY 'pw';
GRANT CREATE ROLE, DROP ROLE ON *.* TO 'm8_role_static'@'%';

-- 4.9 CREATE ROLE/DROP ROLE mixed with portable static privs
CREATE USER 'm8_static_role_mix'@'%' IDENTIFIED BY 'pw';
GRANT SELECT, CREATE ROLE, INSERT, DROP ROLE
  ON *.* TO 'm8_static_role_mix'@'%';

-- 4.10 WITH GRANT OPTION on dynamic privileges is dropped: MariaDB has one
--      grant option per level, which would make every global privilege
--      grantable
CREATE USER 'm8_dyn_grant_opt'@'%' IDENTIFIED BY 'pw';
GRANT BINLOG_ADMIN, CONNECTION_ADMIN ON *.* TO 'm8_dyn_grant_opt'@'%' WITH GRANT OPTION;

-- 4.11 FLUSH_* family - all dropped + warn (RELOAD would over-grant)
CREATE USER 'm8_dyn_flush_collapse'@'%' IDENTIFIED BY 'pw';
GRANT FLUSH_OPTIMIZER_COSTS, FLUSH_STATUS, FLUSH_TABLES, FLUSH_USER_RESOURCES
  ON *.* TO 'm8_dyn_flush_collapse'@'%';

-- Section 5: identifier-parsing false-positive guards

CREATE USER 'm8_col_priv'@'%' IDENTIFIED BY 'pw';
GRANT SELECT(col_a, col_underscore_b), UPDATE(col_a)
  ON m8_db.tab TO 'm8_col_priv'@'%';

CREATE USER 'm8_db_underscore'@'%' IDENTIFIED BY 'pw';
GRANT SELECT ON `m8_under_score_db`.* TO 'm8_db_underscore'@'%';

CREATE USER 'm8 edge space'@'%' IDENTIFIED BY 'pw';
GRANT SELECT ON m8_db.* TO 'm8 edge space'@'%';

-- Internal @ must not be treated as host separator.
CREATE USER 'm8_edge@in_name'@'%' IDENTIFIED BY 'pw';
GRANT SELECT ON m8_db.* TO 'm8_edge@in_name'@'%';

CREATE USER 'm8_edge_wildcard_host'@'192.168.%.%' IDENTIFIED BY 'pw';
GRANT SELECT ON m8_db.* TO 'm8_edge_wildcard_host'@'192.168.%.%';

-- Section 6: scope coverage

CREATE USER 'm8_all_privs'@'%' IDENTIFIED BY 'pw';
GRANT ALL PRIVILEGES ON *.* TO 'm8_all_privs'@'%' WITH GRANT OPTION;

CREATE USER 'm8_multiscope'@'%' IDENTIFIED BY 'pw';
GRANT PROCESS        ON *.*       TO 'm8_multiscope'@'%';
GRANT SELECT         ON m8_db.*   TO 'm8_multiscope'@'%';
GRANT INSERT, UPDATE ON m8_db.tab TO 'm8_multiscope'@'%';
GRANT SELECT(col_a)  ON m8_db.tab2 TO 'm8_multiscope'@'%';

-- Section 7: routine privileges
CREATE USER 'm8_proc_exec'@'%' IDENTIFIED BY 'pw';
GRANT EXECUTE ON PROCEDURE m8_db.proc_one TO 'm8_proc_exec'@'%';

CREATE USER 'm8_func_exec'@'%' IDENTIFIED BY 'pw';
GRANT EXECUTE ON FUNCTION m8_db.func_one TO 'm8_func_exec'@'%';

CREATE USER 'm8_routine_admin'@'%' IDENTIFIED BY 'pw';
GRANT CREATE ROUTINE, ALTER ROUTINE, EXECUTE
  ON m8_db.* TO 'm8_routine_admin'@'%';

-- Section 8: PROXY
CREATE USER 'm8_proxy_target'@'%' IDENTIFIED BY 'pw';
CREATE USER 'm8_proxy_user'@'%'   IDENTIFIED BY 'pw';
GRANT PROXY ON 'm8_proxy_target'@'%' TO 'm8_proxy_user'@'%';

-- Section 9: USAGE-only (no privileges granted)
CREATE USER 'm8_usage_only'@'%' IDENTIFIED BY 'pw';

-- Section 10: CREATE USER extension coverage

CREATE USER 'm8_lock'@'%'   IDENTIFIED BY 'pw' ACCOUNT LOCK;
CREATE USER 'm8_unlock'@'%' IDENTIFIED BY 'pw' ACCOUNT UNLOCK;

CREATE USER 'm8_default_role_one'@'%' IDENTIFIED BY 'pw';
GRANT 'm8_default_a' TO 'm8_default_role_one'@'%';
SET DEFAULT ROLE 'm8_default_a' TO 'm8_default_role_one'@'%';

CREATE USER 'm8_default_role_many'@'%' IDENTIFIED BY 'pw';
GRANT 'm8_default_a','m8_default_b' TO 'm8_default_role_many'@'%';
SET DEFAULT ROLE 'm8_default_a','m8_default_b' TO 'm8_default_role_many'@'%';

CREATE USER 'm8_pwd_history'@'%'         IDENTIFIED BY 'pw' PASSWORD HISTORY 5;
CREATE USER 'm8_pwd_reuse'@'%'           IDENTIFIED BY 'pw' PASSWORD REUSE INTERVAL 365 DAY;
CREATE USER 'm8_pwd_require_current'@'%' IDENTIFIED BY 'pw' PASSWORD REQUIRE CURRENT;

CREATE USER 'm8_pwd_expire_now'@'%'      IDENTIFIED BY 'pw' PASSWORD EXPIRE;
CREATE USER 'm8_pwd_expire_never'@'%'    IDENTIFIED BY 'pw' PASSWORD EXPIRE NEVER;
CREATE USER 'm8_pwd_expire_interval'@'%' IDENTIFIED BY 'pw' PASSWORD EXPIRE INTERVAL 90 DAY;
CREATE USER 'm8_pwd_expire_default'@'%'  IDENTIFIED BY 'pw' PASSWORD EXPIRE DEFAULT;

CREATE USER 'm8_max_conn'@'%' IDENTIFIED BY 'pw'
  WITH MAX_USER_CONNECTIONS 10 MAX_QUERIES_PER_HOUR 1000;

CREATE USER 'm8_resource_full'@'%' IDENTIFIED BY 'pw'
  WITH MAX_QUERIES_PER_HOUR 5000
       MAX_UPDATES_PER_HOUR 500
       MAX_CONNECTIONS_PER_HOUR 100
       MAX_USER_CONNECTIONS 25;

CREATE USER 'm8_require_ssl'@'%'    IDENTIFIED BY 'pw' REQUIRE SSL;
CREATE USER 'm8_require_x509'@'%'   IDENTIFIED BY 'pw' REQUIRE X509;
CREATE USER 'm8_require_cipher'@'%' IDENTIFIED BY 'pw'
  REQUIRE CIPHER 'ECDHE-RSA-AES128-GCM-SHA256';

CREATE USER 'm8_failed_login'@'%' IDENTIFIED BY 'pw'
  FAILED_LOGIN_ATTEMPTS 3 PASSWORD_LOCK_TIME 1;

CREATE USER 'm8_with_comment'@'%' IDENTIFIED BY 'pw'
  COMMENT 'Migration test user with embedded ''quote'' character';

CREATE USER 'm8_with_attribute'@'%' IDENTIFIED BY 'pw'
  ATTRIBUTE '{"team":"db_migration","contact":"alice@example.com"}';

-- Combined: kept (REQUIRE, WITH MAX_, ACCOUNT LOCK) and dropped
-- (DEFAULT ROLE, PASSWORD HISTORY/REUSE) clauses in one CREATE USER.
CREATE USER 'm8_combined'@'%' IDENTIFIED BY 'pw'
  DEFAULT ROLE 'm8_default_a'
  REQUIRE SSL
  WITH MAX_USER_CONNECTIONS 5
  PASSWORD HISTORY 3
  PASSWORD REUSE INTERVAL 90 DAY
  ACCOUNT LOCK;
GRANT 'm8_default_a' TO 'm8_combined'@'%';
GRANT SELECT ON m8_db.* TO 'm8_combined'@'%';

-- Section 11: dedup stress — five users share m8_shared_role
CREATE USER 'm8_shared_user_1'@'%' IDENTIFIED BY 'pw';
CREATE USER 'm8_shared_user_2'@'%' IDENTIFIED BY 'pw';
CREATE USER 'm8_shared_user_3'@'%' IDENTIFIED BY 'pw';
CREATE USER 'm8_shared_user_4'@'%' IDENTIFIED BY 'pw';
CREATE USER 'm8_shared_user_5'@'%' IDENTIFIED BY 'pw';
GRANT 'm8_shared_role' TO 'm8_shared_user_1'@'%';
GRANT 'm8_shared_role' TO 'm8_shared_user_2'@'%' WITH ADMIN OPTION;
GRANT 'm8_shared_role' TO 'm8_shared_user_3'@'%';
GRANT 'm8_shared_role' TO 'm8_shared_user_4'@'%';
GRANT 'm8_shared_role' TO 'm8_shared_user_5'@'%' WITH ADMIN OPTION;

-- m8_default_a is also handed to two of the shared users, so it too is
-- a role held by several users (m8_default_role_one/_many, m8_combined,
-- and these two).
GRANT 'm8_default_a' TO 'm8_shared_user_1'@'%';
GRANT 'm8_default_a' TO 'm8_shared_user_2'@'%';

-- Section 12: cross-cutting role grants
GRANT 'm8_role_a' TO 'm8_simple'@'%' WITH ADMIN OPTION;
GRANT 'm8_role_d' TO 'm8_simple'@'%';

-- Section 13: authentication plugins.
-- No mysql_native_password user: disabled in MySQL 8.4, removed in 9.0.
-- sha256_password is not shipped by MariaDB: the IDENTIFIED clause is
-- dropped with a # WARNING and ACCOUNT LOCK is forced.
CREATE USER 'm8_auth_sha256'@'%'
  IDENTIFIED WITH sha256_password BY 'pw_sha256';
CREATE USER 'm8_auth_no_pwd'@'%';
GRANT SELECT ON m8_db.* TO 'm8_auth_no_pwd'@'%';
-- caching_sha2_password: the IDENTIFIED clause is kept, so the CREATE USER
-- fails on import unless the auth_mysql_sha2 plugin is loaded.
CREATE USER 'm8_auth_caching_sha2'@'%'
  IDENTIFIED WITH caching_sha2_password BY 'pw_caching';

-- Section 14: MFA - INTENTIONALLY OMITTED.
-- MySQL 8.4 ER 4052 rejects primary auth plugins as factor 2/3, and
-- real MFA plugins (FIDO/Kerberos/LDAP_*) need external infra. The dropped
-- extra factors are covered by the unit test (mysql8_translate-t.cc).

-- Section 15: same name, different hosts (host-aware dedup)
CREATE USER 'm8_host_localhost'@'localhost' IDENTIFIED BY 'pw';
GRANT SELECT ON m8_db.* TO 'm8_host_localhost'@'localhost';

CREATE USER 'm8_host_dual'@'localhost' IDENTIFIED BY 'pw_local';
CREATE USER 'm8_host_dual'@'%'         IDENTIFIED BY 'pw_any';
GRANT SELECT ON m8_db.*    TO 'm8_host_dual'@'localhost';
GRANT SELECT ON m8_db.tab2 TO 'm8_host_dual'@'%';

-- Section 16: identifier edge cases (anonymous + embedded quote)
CREATE USER ''@'localhost' IDENTIFIED BY 'pw_anon';
GRANT USAGE ON *.* TO ''@'localhost';

-- Anonymous-from-anywhere. Real legacy installs sometimes have this.
CREATE USER ''@'%' IDENTIFIED BY 'pw_anon_any';
GRANT USAGE ON *.* TO ''@'%';

-- Embedded single-quote in a user name.
CREATE USER 'O\'Brien'@'%' IDENTIFIED BY 'pw_obrien';
GRANT SELECT ON m8_db.* TO 'O\'Brien'@'%';

-- UTF-8 / non-ASCII identifier. The translator's parser is byte-level,
-- so multibyte chars in identifiers should be transparent.
CREATE USER 'm8_用户'@'%' IDENTIFIED BY 'pw_unicode';
GRANT SELECT ON m8_db.* TO 'm8_用户'@'%';

-- COMMENT values with a literal '*/' (plain, and backslash-escaped).
-- The dump drops COMMENT with a # WARNING that carries the '*/'.
CREATE USER 'm8_starslash_comment'@'%' IDENTIFIED BY 'pw'
  COMMENT 'has */ in body';

CREATE USER 'm8_starslash_escaped'@'%' IDENTIFIED BY 'pw'
  COMMENT 'has \\*/ escaped';

-- Section 17: system-schema grants. GRANT SELECT ON
-- information_schema.* is documented to silently no-op on MySQL
-- (I_S is virtual; no privilege row stored), so omitted here.
CREATE USER 'm8_system_grants'@'%' IDENTIFIED BY 'pw_sys';
GRANT SELECT ON mysql.user           TO 'm8_system_grants'@'%';
GRANT SELECT ON performance_schema.* TO 'm8_system_grants'@'%';

-- Section 18: partial REVOKE (covers translate_mysql8_revoke).
-- SHOW GRANTS emits a REVOKE line only for a db-level partial revoke of a
-- STATIC privilege, with partial_revokes=ON (needs SYSTEM_VARIABLES_ADMIN
-- or SUPER). Dynamic privileges are *.* only and make the REVOKE fail.
-- Check the dump has a '# WARNING: partial REVOKE' line after recapture.
-- Reset afterwards: SET GLOBAL partial_revokes = OFF;
SET GLOBAL partial_revokes = ON;
CREATE USER 'm8_partial_revoke'@'%' IDENTIFIED BY 'pw_partial';
GRANT SELECT, INSERT ON *.* TO 'm8_partial_revoke'@'%';
REVOKE INSERT ON `m8_db`.* FROM 'm8_partial_revoke'@'%';

-- Section 19: hybrid account - a login user that is also granted to another
-- account as a role (MySQL has no separate role objects). It must migrate
-- as a user with its password and as a role with its grants and members.
CREATE USER 'm8_hybrid'@'%' IDENTIFIED BY 'pw_hybrid';
GRANT SELECT ON m8_db.tab TO 'm8_hybrid'@'%';
GRANT 'm8_role_c' TO 'm8_hybrid'@'%';
CREATE USER 'm8_hybrid_member'@'%' IDENTIFIED BY 'pw_member';
GRANT 'm8_hybrid'@'%' TO 'm8_hybrid_member'@'%';

-- Section 20: one role name on two hosts. MariaDB roles have no host, so the
-- two must merge into a single role and the shared member is granted once.
CREATE ROLE 'm8_role_dual'@'h1', 'm8_role_dual'@'h2';
GRANT SELECT ON m8_db.tab  TO 'm8_role_dual'@'h1';
GRANT INSERT ON m8_db.tab  TO 'm8_role_dual'@'h2';
CREATE USER 'm8_dual_member'@'%' IDENTIFIED BY 'pw_dual';
GRANT 'm8_role_dual'@'h1', 'm8_role_dual'@'h2' TO 'm8_dual_member'@'%';

-- Section 21: role granted to an account missing from mysql.user (an
-- orphaned role_edges row), and a non-empty mandatory_roles. Each makes the
-- dump print a warning.
CREATE USER 'm8_orphan_member'@'%' IDENTIFIED BY 'pw_orphan';
GRANT 'm8_role_c' TO 'm8_orphan_member'@'%';
DELETE FROM mysql.user WHERE User = 'm8_orphan_member';
SET GLOBAL mandatory_roles = 'm8_role_c';
-- Reset after capture: SET GLOBAL mandatory_roles = '';
