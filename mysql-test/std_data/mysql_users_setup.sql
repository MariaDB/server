-- =====================================================================
-- Test fixture: MySQL 8.0+ state for round-trip testing of mariadb-dump.
--
-- Workflow:
--   1. mysql -uroot -p < mysql_users_setup.sql   (on a fresh MySQL 8.0+)
--   2. ./client/mariadb-dump --plugin-dir=$BUILD/libmariadb \
--        -h <host> -P <port> -u root --system=users > mysql_users_dump.sql
--   3. Make the dump reproducible and printable. Set the "-- Host:" line to
--      "localhost", and replace each random caching_sha2_password hash, which
--      has control bytes, with a fixed one (src_auth_caching_sha2 keeps its
--      hash: the import tests log in with it):
--        perl -pi -e 's/(AS \x27\$A\$[0-9A-Z]{3}\$)(?:[^\x27\\]|\\.)*\x27/$1abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq\x27/ unless /src_auth_caching_sha2/' mysql_users_dump.sql
--   4. Commit the captured dump.
--
-- Step 1 needs a SUPER user (or SYSTEM_VARIABLES_ADMIN): Section 18 sets
-- partial_revokes. The committed capture was taken as root; src_dumper of
-- Section 2 has the privileges a non-root dump needs.
--
-- Names describe the case they exercise (src_dyn_mapped, ...) so a failing
-- import test points at it. Re-runs are safe (Section 0 drops what it created).
-- COMMENT/ATTRIBUTE need MySQL 8.0.21+, FAILED_LOGIN_ATTEMPTS 8.0.19+.
-- =====================================================================

-- Section 0: cleanup. Drops the databases and every account and role named
-- src*, and the others the sections create: the anonymous users, O'Brien and
-- the roles of Section 22.
SET GLOBAL mandatory_roles = '';
DELETE FROM mysql.role_edges WHERE TO_USER = 'src_orphan_member';
-- Section 21 of an earlier run deleted its account from mysql.user only
FLUSH PRIVILEGES;

DROP DATABASE IF EXISTS src_db;
DROP DATABASE IF EXISTS `src_under_score_db`;
CREATE DATABASE src_db;

DELIMITER $$
CREATE PROCEDURE src_db.drop_accounts()
BEGIN
  DECLARE done BOOL DEFAULT FALSE;
  DECLARE account TEXT;
  DECLARE accounts CURSOR FOR
    SELECT CONCAT(QUOTE(User), '@', QUOTE(Host)) FROM mysql.user
    WHERE User LIKE 'src%'
       OR User IN ('', 'O\'Brien', 'Public', 'none', 'Public_0001');
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done= TRUE;
  OPEN accounts;
  REPEAT
    FETCH accounts INTO account;
    IF NOT done THEN
      SET @drop_account= CONCAT('DROP USER ', account);
      PREPARE stmt FROM @drop_account;
      EXECUTE stmt;
      DEALLOCATE PREPARE stmt;
    END IF;
  UNTIL done END REPEAT;
  CLOSE accounts;
END$$
DELIMITER ;
CALL src_db.drop_accounts();
DROP PROCEDURE src_db.drop_accounts;

-- Section 1: schemas, tables, routines (src_db is created by Section 0)
CREATE TABLE src_db.tab (col_a INT, col_underscore_b INT, plain INT);
CREATE TABLE src_db.tab2 (id INT PRIMARY KEY, payload TEXT);
CREATE VIEW src_db.v AS SELECT col_a FROM src_db.tab;

DELIMITER $$
CREATE PROCEDURE src_db.proc_one(IN x INT) BEGIN SELECT x; END$$
CREATE FUNCTION  src_db.func_one(x INT) RETURNS INT DETERMINISTIC
  BEGIN RETURN x * 2; END$$
DELIMITER ;

CREATE DATABASE `src_under_score_db`;
CREATE TABLE `src_under_score_db`.t (a INT);

-- Section 2: dumping user
CREATE USER 'src_dumper'@'%' IDENTIFIED BY 'dumper_pw';
GRANT SELECT, LOCK TABLES, SHOW VIEW, EVENT, TRIGGER,
      RELOAD, PROCESS, REPLICATION CLIENT, SHOW DATABASES, CREATE USER
  ON *.* TO 'src_dumper'@'%';
GRANT SELECT ON mysql.* TO 'src_dumper'@'%';

-- Section 3: roles — 3-level admin chain + a 5-user-shared role
CREATE ROLE 'src_role_a','src_role_b','src_role_c','src_role_d';
CREATE ROLE 'src_default_a','src_default_b';
CREATE ROLE 'src_shared_role','src_admin_chain_root';

GRANT 'src_role_b' TO 'src_role_a' WITH ADMIN OPTION;
GRANT 'src_role_c' TO 'src_role_b' WITH ADMIN OPTION;
GRANT SELECT         ON src_db.* TO 'src_role_a';
GRANT INSERT         ON src_db.* TO 'src_role_b';
GRANT UPDATE         ON src_db.* TO 'src_role_c';
GRANT SHOW DATABASES ON *.*    TO 'src_role_d';
GRANT 'src_role_a' TO 'src_admin_chain_root' WITH ADMIN OPTION;
GRANT SELECT  ON src_db.tab               TO 'src_shared_role';
GRANT EXECUTE ON PROCEDURE src_db.proc_one TO 'src_shared_role';

-- Orphan role (no members) with '*/' in its name: it lands as a locked user.
-- Its BACKUP_ADMIN grant is unmapped, so it is dropped with a # WARNING.
CREATE ROLE `src_role*/starslash`;
GRANT BACKUP_ADMIN ON *.* TO `src_role*/starslash`;

-- Section 4: GRANT translator coverage

-- 4.1 plain SELECT only -> translator NOT triggered
CREATE USER 'src_simple'@'%' IDENTIFIED BY 'pw';
GRANT SELECT ON src_db.* TO 'src_simple'@'%';

-- 4.2 only static privileges across multiple grants
CREATE USER 'src_static_priv'@'%' IDENTIFIED BY 'pw';
GRANT SELECT, INSERT, UPDATE, DELETE ON src_db.* TO 'src_static_priv'@'%';
GRANT RELOAD, PROCESS                ON *.*    TO 'src_static_priv'@'%';

-- 4.3 single dynamic priv WITH MariaDB equivalent
CREATE USER 'src_dyn_mapped'@'%' IDENTIFIED BY 'pw';
GRANT BINLOG_ADMIN ON *.* TO 'src_dyn_mapped'@'%';

-- 4.4 single dynamic priv with NO MariaDB equivalent -> dropped + warn
CREATE USER 'src_dyn_unmapped'@'%' IDENTIFIED BY 'pw';
GRANT BACKUP_ADMIN ON *.* TO 'src_dyn_unmapped'@'%';

-- 4.5 mixed mapped + unmapped dynamic privs
CREATE USER 'src_dyn_mixed'@'%' IDENTIFIED BY 'pw';
GRANT BACKUP_ADMIN, BINLOG_ADMIN, CONNECTION_ADMIN, AUDIT_ADMIN,
      REPLICATION_APPLIER, SHOW_ROUTINE
  ON *.* TO 'src_dyn_mixed'@'%';

-- 4.6 all-unmapped -> entire MariaDB GRANT line suppressed
CREATE USER 'src_dyn_all_dropped'@'%' IDENTIFIED BY 'pw';
GRANT BACKUP_ADMIN, AUDIT_ADMIN, CLONE_ADMIN ON *.* TO 'src_dyn_all_dropped'@'%';

-- 4.7 static + dynamic in one GRANT
CREATE USER 'src_static_dyn_mix'@'%' IDENTIFIED BY 'pw';
GRANT SELECT, BINLOG_ADMIN, BACKUP_ADMIN ON *.* TO 'src_static_dyn_mix'@'%';

-- 4.8 MySQL-only role privs (CREATE ROLE / DROP ROLE)
CREATE USER 'src_role_static'@'%' IDENTIFIED BY 'pw';
GRANT CREATE ROLE, DROP ROLE ON *.* TO 'src_role_static'@'%';

-- 4.9 CREATE ROLE/DROP ROLE mixed with portable static privs
CREATE USER 'src_static_role_mix'@'%' IDENTIFIED BY 'pw';
GRANT SELECT, CREATE ROLE, INSERT, DROP ROLE
  ON *.* TO 'src_static_role_mix'@'%';

-- 4.10 WITH GRANT OPTION on dynamic privileges is dropped: MariaDB has one
--      grant option per level, which would make every global privilege
--      grantable. If the account has a static one on *.* too, that is
--      dropped when a dynamic privilege has none (4.12)
CREATE USER 'src_dyn_grant_opt'@'%' IDENTIFIED BY 'pw';
GRANT BINLOG_ADMIN, CONNECTION_ADMIN ON *.* TO 'src_dyn_grant_opt'@'%' WITH GRANT OPTION;

-- 4.11 FLUSH_* family - all dropped + warn (RELOAD would over-grant)
CREATE USER 'src_dyn_flush_collapse'@'%' IDENTIFIED BY 'pw';
GRANT FLUSH_OPTIMIZER_COSTS, FLUSH_STATUS, FLUSH_TABLES, FLUSH_USER_RESOURCES
  ON *.* TO 'src_dyn_flush_collapse'@'%';

-- 4.12 static WITH GRANT OPTION, one dynamic privilege without it: MySQL
--      lets the account grant the static ones only, MariaDB would grant both
CREATE USER 'src_opt_static_dyn'@'%' IDENTIFIED BY 'pw';
GRANT SELECT ON *.* TO 'src_opt_static_dyn'@'%' WITH GRANT OPTION;
GRANT CONNECTION_ADMIN ON *.* TO 'src_opt_static_dyn'@'%';

--      the dynamic privilege has the option too: the static one keeps it
CREATE USER 'src_opt_static_dyn_opt'@'%' IDENTIFIED BY 'pw';
GRANT SELECT ON *.* TO 'src_opt_static_dyn_opt'@'%' WITH GRANT OPTION;
GRANT CONNECTION_ADMIN ON *.* TO 'src_opt_static_dyn_opt'@'%' WITH GRANT OPTION;

-- Section 5: identifier-parsing false-positive guards

CREATE USER 'src_col_priv'@'%' IDENTIFIED BY 'pw';
GRANT SELECT(col_a, col_underscore_b), UPDATE(col_a)
  ON src_db.tab TO 'src_col_priv'@'%';

CREATE USER 'src_db_underscore'@'%' IDENTIFIED BY 'pw';
GRANT SELECT ON `src_under_score_db`.* TO 'src_db_underscore'@'%';

CREATE USER 'src edge space'@'%' IDENTIFIED BY 'pw';
GRANT SELECT ON src_db.* TO 'src edge space'@'%';

-- Internal @ must not be treated as host separator.
CREATE USER 'src_edge@in_name'@'%' IDENTIFIED BY 'pw';
GRANT SELECT ON src_db.* TO 'src_edge@in_name'@'%';

CREATE USER 'src_edge_wildcard_host'@'192.168.%.%' IDENTIFIED BY 'pw';
GRANT SELECT ON src_db.* TO 'src_edge_wildcard_host'@'192.168.%.%';

-- Section 6: scope coverage

CREATE USER 'src_all_privs'@'%' IDENTIFIED BY 'pw';
GRANT ALL PRIVILEGES ON *.* TO 'src_all_privs'@'%' WITH GRANT OPTION;

CREATE USER 'src_multiscope'@'%' IDENTIFIED BY 'pw';
GRANT PROCESS        ON *.*       TO 'src_multiscope'@'%';
GRANT SELECT         ON src_db.*   TO 'src_multiscope'@'%';
GRANT INSERT, UPDATE ON src_db.tab TO 'src_multiscope'@'%';
GRANT SELECT(col_a)  ON src_db.tab2 TO 'src_multiscope'@'%';

-- Section 7: routine privileges
CREATE USER 'src_proc_exec'@'%' IDENTIFIED BY 'pw';
GRANT EXECUTE ON PROCEDURE src_db.proc_one TO 'src_proc_exec'@'%';

CREATE USER 'src_func_exec'@'%' IDENTIFIED BY 'pw';
GRANT EXECUTE ON FUNCTION src_db.func_one TO 'src_func_exec'@'%';

CREATE USER 'src_routine_admin'@'%' IDENTIFIED BY 'pw';
GRANT CREATE ROUTINE, ALTER ROUTINE, EXECUTE
  ON src_db.* TO 'src_routine_admin'@'%';

-- Section 8: PROXY
CREATE USER 'src_proxy_target'@'%' IDENTIFIED BY 'pw';
CREATE USER 'src_proxy_user'@'%'   IDENTIFIED BY 'pw';
GRANT PROXY ON 'src_proxy_target'@'%' TO 'src_proxy_user'@'%';

-- Section 9: USAGE-only (no privileges granted)
CREATE USER 'src_usage_only'@'%' IDENTIFIED BY 'pw';

-- Section 10: CREATE USER extension coverage

CREATE USER 'src_lock'@'%'   IDENTIFIED BY 'pw' ACCOUNT LOCK;
CREATE USER 'src_unlock'@'%' IDENTIFIED BY 'pw' ACCOUNT UNLOCK;

CREATE USER 'src_default_role_one'@'%' IDENTIFIED BY 'pw';
GRANT 'src_default_a' TO 'src_default_role_one'@'%';
SET DEFAULT ROLE 'src_default_a' TO 'src_default_role_one'@'%';

CREATE USER 'src_default_role_many'@'%' IDENTIFIED BY 'pw';
GRANT 'src_default_a','src_default_b' TO 'src_default_role_many'@'%';
SET DEFAULT ROLE 'src_default_a','src_default_b' TO 'src_default_role_many'@'%';

CREATE USER 'src_pwd_history'@'%'         IDENTIFIED BY 'pw' PASSWORD HISTORY 5;
CREATE USER 'src_pwd_reuse'@'%'           IDENTIFIED BY 'pw' PASSWORD REUSE INTERVAL 365 DAY;
CREATE USER 'src_pwd_require_current'@'%' IDENTIFIED BY 'pw' PASSWORD REQUIRE CURRENT;

CREATE USER 'src_pwd_expire_now'@'%'      IDENTIFIED BY 'pw' PASSWORD EXPIRE;
CREATE USER 'src_pwd_expire_never'@'%'    IDENTIFIED BY 'pw' PASSWORD EXPIRE NEVER;
CREATE USER 'src_pwd_expire_interval'@'%' IDENTIFIED BY 'pw' PASSWORD EXPIRE INTERVAL 90 DAY;
CREATE USER 'src_pwd_expire_default'@'%'  IDENTIFIED BY 'pw' PASSWORD EXPIRE DEFAULT;

CREATE USER 'src_max_conn'@'%' IDENTIFIED BY 'pw'
  WITH MAX_USER_CONNECTIONS 10 MAX_QUERIES_PER_HOUR 1000;

CREATE USER 'src_resource_full'@'%' IDENTIFIED BY 'pw'
  WITH MAX_QUERIES_PER_HOUR 5000
       MAX_UPDATES_PER_HOUR 500
       MAX_CONNECTIONS_PER_HOUR 100
       MAX_USER_CONNECTIONS 25;

CREATE USER 'src_require_ssl'@'%'    IDENTIFIED BY 'pw' REQUIRE SSL;
CREATE USER 'src_require_x509'@'%'   IDENTIFIED BY 'pw' REQUIRE X509;
CREATE USER 'src_require_cipher'@'%' IDENTIFIED BY 'pw'
  REQUIRE CIPHER 'ECDHE-RSA-AES128-GCM-SHA256';

CREATE USER 'src_failed_login'@'%' IDENTIFIED BY 'pw'
  FAILED_LOGIN_ATTEMPTS 3 PASSWORD_LOCK_TIME 1;

CREATE USER 'src_with_comment'@'%' IDENTIFIED BY 'pw'
  COMMENT 'Migration test user with embedded ''quote'' character';

CREATE USER 'src_with_attribute'@'%' IDENTIFIED BY 'pw'
  ATTRIBUTE '{"team":"db_migration","contact":"alice@example.com"}';

-- Combined: kept (REQUIRE, WITH MAX_, ACCOUNT LOCK) and dropped
-- (DEFAULT ROLE, PASSWORD HISTORY/REUSE) clauses in one CREATE USER.
CREATE USER 'src_combined'@'%' IDENTIFIED BY 'pw'
  DEFAULT ROLE 'src_default_a'
  REQUIRE SSL
  WITH MAX_USER_CONNECTIONS 5
  PASSWORD HISTORY 3
  PASSWORD REUSE INTERVAL 90 DAY
  ACCOUNT LOCK;
GRANT 'src_default_a' TO 'src_combined'@'%';
GRANT SELECT ON src_db.* TO 'src_combined'@'%';

-- Section 11: dedup stress — five users share src_shared_role
CREATE USER 'src_shared_user_1'@'%' IDENTIFIED BY 'pw';
CREATE USER 'src_shared_user_2'@'%' IDENTIFIED BY 'pw';
CREATE USER 'src_shared_user_3'@'%' IDENTIFIED BY 'pw';
CREATE USER 'src_shared_user_4'@'%' IDENTIFIED BY 'pw';
CREATE USER 'src_shared_user_5'@'%' IDENTIFIED BY 'pw';
GRANT 'src_shared_role' TO 'src_shared_user_1'@'%';
GRANT 'src_shared_role' TO 'src_shared_user_2'@'%' WITH ADMIN OPTION;
GRANT 'src_shared_role' TO 'src_shared_user_3'@'%';
GRANT 'src_shared_role' TO 'src_shared_user_4'@'%';
GRANT 'src_shared_role' TO 'src_shared_user_5'@'%' WITH ADMIN OPTION;

-- src_default_a is also handed to two of the shared users, so it too is
-- a role held by several users (src_default_role_one/_many, src_combined,
-- and these two).
GRANT 'src_default_a' TO 'src_shared_user_1'@'%';
GRANT 'src_default_a' TO 'src_shared_user_2'@'%';

-- Section 12: cross-cutting role grants
GRANT 'src_role_a' TO 'src_simple'@'%' WITH ADMIN OPTION;
GRANT 'src_role_d' TO 'src_simple'@'%';

-- Section 13: authentication plugins.
-- No mysql_native_password user: disabled in MySQL 8.4, removed in 9.0.
-- sha256_password is not shipped by MariaDB: the IDENTIFIED clause is
-- dropped with a # WARNING and ACCOUNT LOCK is forced.
CREATE USER 'src_auth_sha256'@'%'
  IDENTIFIED WITH sha256_password BY 'pw_sha256';
CREATE USER 'src_auth_no_pwd'@'%';
GRANT SELECT ON src_db.* TO 'src_auth_no_pwd'@'%';
-- caching_sha2_password: the IDENTIFIED clause is kept, so the CREATE USER
-- fails on import unless the auth_mysql_sha2 plugin is loaded. The hash is of
-- 'pw_caching', with a printable salt (a random one has control bytes).
CREATE USER 'src_auth_caching_sha2'@'%'
  IDENTIFIED WITH caching_sha2_password
  AS '$A$005$abcdefghijklmnopqrstxwQelgjWfa4IIO4KqUZbkx7NT5KrPLk9MAF5NI3WkwD';

-- Section 14: MFA - INTENTIONALLY OMITTED.
-- MySQL 8.4 ER 4052 rejects primary auth plugins as factor 2/3, and
-- real MFA plugins (FIDO/Kerberos/LDAP_*) need external infra. The dropped
-- extra factors are covered by the unit test (mysql_translate-t.cc).

-- Section 15: same name, different hosts (host-aware dedup)
CREATE USER 'src_host_localhost'@'localhost' IDENTIFIED BY 'pw';
GRANT SELECT ON src_db.* TO 'src_host_localhost'@'localhost';

CREATE USER 'src_host_dual'@'localhost' IDENTIFIED BY 'pw_local';
CREATE USER 'src_host_dual'@'%'         IDENTIFIED BY 'pw_any';
GRANT SELECT ON src_db.*    TO 'src_host_dual'@'localhost';
GRANT SELECT ON src_db.tab2 TO 'src_host_dual'@'%';

-- Section 16: identifier edge cases (anonymous + embedded quote)
CREATE USER ''@'localhost' IDENTIFIED BY 'pw_anon';
GRANT USAGE ON *.* TO ''@'localhost';

-- Anonymous-from-anywhere. Real legacy installs sometimes have this.
CREATE USER ''@'%' IDENTIFIED BY 'pw_anon_any';
GRANT USAGE ON *.* TO ''@'%';

-- Embedded single-quote in a user name.
CREATE USER 'O\'Brien'@'%' IDENTIFIED BY 'pw_obrien';
GRANT SELECT ON src_db.* TO 'O\'Brien'@'%';

-- UTF-8 / non-ASCII identifier. The translator's parser is byte-level,
-- so multibyte chars in identifiers should be transparent.
CREATE USER 'src_用户'@'%' IDENTIFIED BY 'pw_unicode';
GRANT SELECT ON src_db.* TO 'src_用户'@'%';

-- COMMENT values with a literal '*/' (plain, and backslash-escaped).
-- The dump drops COMMENT with a # WARNING that carries the '*/'.
CREATE USER 'src_starslash_comment'@'%' IDENTIFIED BY 'pw'
  COMMENT 'has */ in body';

CREATE USER 'src_starslash_escaped'@'%' IDENTIFIED BY 'pw'
  COMMENT 'has \\*/ escaped';

-- Section 17: system-schema grants. GRANT SELECT ON
-- information_schema.* is documented to silently no-op on MySQL
-- (I_S is virtual; no privilege row stored), so omitted here.
CREATE USER 'src_system_grants'@'%' IDENTIFIED BY 'pw_sys';
GRANT SELECT ON mysql.user           TO 'src_system_grants'@'%';
GRANT SELECT ON performance_schema.* TO 'src_system_grants'@'%';

-- Section 18: partial REVOKE (covers translate_mysql_revoke).
-- SHOW GRANTS emits a REVOKE line only for a db-level partial revoke of a
-- STATIC privilege, with partial_revokes=ON (needs SYSTEM_VARIABLES_ADMIN
-- or SUPER). Dynamic privileges are *.* only and make the REVOKE fail.
-- Check the dump has a DENY line after recapture.
-- Reset afterwards: SET GLOBAL partial_revokes = OFF;
SET GLOBAL partial_revokes = ON;
CREATE USER 'src_partial_revoke'@'%' IDENTIFIED BY 'pw_partial';
GRANT SELECT, INSERT ON *.* TO 'src_partial_revoke'@'%';
REVOKE INSERT ON `src_db`.* FROM 'src_partial_revoke'@'%';

-- Section 19: hybrid account - a login user that is also granted to another
-- account as a role (MySQL has no separate role objects). It must migrate
-- as a user with its password and grants, and as a role with its grants and
-- members. The role is not the user's default role.
CREATE USER 'src_hybrid'@'%' IDENTIFIED BY 'pw_hybrid';
GRANT SELECT ON src_db.tab TO 'src_hybrid'@'%';
GRANT 'src_role_c' TO 'src_hybrid'@'%';
CREATE USER 'src_hybrid_member'@'%' IDENTIFIED BY 'pw_member';
GRANT 'src_hybrid'@'%' TO 'src_hybrid_member'@'%';

-- Section 20: one role name on two hosts. MariaDB roles have no host, so
-- each becomes its own role (src_role_dual_NNNN) and the shared member
-- is granted both.
CREATE ROLE 'src_role_dual'@'h1', 'src_role_dual'@'h2';
GRANT SELECT ON src_db.tab  TO 'src_role_dual'@'h1';
GRANT INSERT ON src_db.tab  TO 'src_role_dual'@'h2';
CREATE USER 'src_dual_member'@'%' IDENTIFIED BY 'pw_dual';
GRANT 'src_role_dual'@'h1', 'src_role_dual'@'h2' TO 'src_dual_member'@'%';

-- Section 21: role granted to an account missing from mysql.user (an
-- orphaned role_edges row), which makes the dump print a warning, and a
-- mandatory role, which the dump grants to PUBLIC. It has no privileges, as
-- they would reach every account.
CREATE ROLE 'src_role_mandatory';
CREATE USER 'src_orphan_member'@'%' IDENTIFIED BY 'pw_orphan';
GRANT 'src_role_c' TO 'src_orphan_member'@'%';
DELETE FROM mysql.user WHERE User = 'src_orphan_member';
SET GLOBAL mandatory_roles = 'src_role_mandatory';
-- Reset after capture: SET GLOBAL mandatory_roles = '';

-- Section 22: role names MariaDB cannot use as they are. PUBLIC and NONE are
-- reserved in any letter case, and Public_0001 is the name the dump gives
-- Public (accounts are taken in binary order, so Public comes first). Each is
-- renamed, and its privileges and its member follow.
CREATE ROLE 'Public', 'none', 'Public_0001';
GRANT INSERT ON src_db.tab2 TO 'Public';
GRANT UPDATE ON src_db.tab2 TO 'none';
GRANT DELETE ON src_db.tab2 TO 'Public_0001';
CREATE USER 'src_name_member'@'%' IDENTIFIED BY 'pw_name';
GRANT 'Public', 'none', 'Public_0001' TO 'src_name_member'@'%';

-- Section 23: default roles of a role account: one, and several. A role cannot
-- log in, so the dump does not carry them over.
CREATE ROLE 'src_role_dflt_one', 'src_role_dflt_two';
CREATE ROLE 'src_role_dflt_x', 'src_role_dflt_y';
GRANT 'src_role_dflt_x' TO 'src_role_dflt_one';
SET DEFAULT ROLE 'src_role_dflt_x' TO 'src_role_dflt_one';
GRANT 'src_role_dflt_x', 'src_role_dflt_y' TO 'src_role_dflt_two';
SET DEFAULT ROLE 'src_role_dflt_x', 'src_role_dflt_y' TO 'src_role_dflt_two';
CREATE USER 'src_dflt_member'@'%' IDENTIFIED BY 'pw_dflt';
GRANT 'src_role_dflt_one', 'src_role_dflt_two' TO 'src_dflt_member'@'%';
