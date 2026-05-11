/*M!999999\- enable the sandbox mode */ 
-- MariaDB dump 10.20-13.1.2-MariaDB, for Linux (x86_64)
--
-- Host: localhost    Database: 
-- ------------------------------------------------------
-- Server version	26.7.0

SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT;
SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS;
SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION;
SET NAMES utf8mb4;
SET @OLD_TIME_ZONE=@@TIME_ZONE;
SET TIME_ZONE='+00:00';
SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0;
SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0;
SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO';
/*M!100616 SET @OLD_NOTE_VERBOSITY=@@NOTE_VERBOSITY, NOTE_VERBOSITY=0 */;
CREATE USER ``@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `O'Brien`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `Public`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `Public_0001`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `none`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src edge space`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_admin_chain_root`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_all_privs`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_auth_caching_sha2`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$005$abcdefghijklmnopqrstxwQelgjWfa4IIO4KqUZbkx7NT5KrPLk9MAF5NI3WkwD' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_auth_no_pwd`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_auth_sha256`@`%` REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;
# WARNING: auth plugin 'sha256_password' not shipped with MariaDB; user created with no password and ACCOUNT LOCK - set a password and ACCOUNT UNLOCK before login
CREATE USER `src_col_priv`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_combined`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE SSL WITH MAX_USER_CONNECTIONS 5 PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;
# WARNING: password policy clause dropped (no MariaDB equivalent): PASSWORD HISTORY 3
# WARNING: password policy clause dropped (no MariaDB equivalent): PASSWORD REUSE INTERVAL 90 DAY
CREATE USER `src_db_underscore`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_default_a`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_default_b`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_default_role_many`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_default_role_one`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_dflt_member`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_dual_member`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_dumper`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_dyn_all_dropped`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_dyn_flush_collapse`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_dyn_grant_opt`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_dyn_mapped`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_dyn_mixed`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_dyn_unmapped`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_edge@in_name`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_failed_login`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: account option dropped (no MariaDB equivalent): FAILED_LOGIN_ATTEMPTS 3
# WARNING: account option dropped (no MariaDB equivalent): PASSWORD_LOCK_TIME 1
CREATE USER `src_func_exec`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_host_dual`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_hybrid`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_hybrid_member`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_lock`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;
CREATE USER `src_max_conn`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE WITH MAX_QUERIES_PER_HOUR 1000 MAX_USER_CONNECTIONS 10 PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_multiscope`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_name_member`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_opt_static_dyn`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_opt_static_dyn_opt`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_partial_revoke`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_proc_exec`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_proxy_target`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_proxy_user`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_pwd_expire_default`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_pwd_expire_interval`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE INTERVAL 90 DAY ACCOUNT UNLOCK;
CREATE USER `src_pwd_expire_never`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE NEVER ACCOUNT UNLOCK;
CREATE USER `src_pwd_expire_now`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE ACCOUNT UNLOCK;
CREATE USER `src_pwd_history`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: password policy clause dropped (no MariaDB equivalent): PASSWORD HISTORY 5
CREATE USER `src_pwd_require_current`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: password policy clause dropped (no MariaDB equivalent): PASSWORD REQUIRE CURRENT
CREATE USER `src_pwd_reuse`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: password policy clause dropped (no MariaDB equivalent): PASSWORD REUSE INTERVAL 365 DAY
CREATE USER `src_require_cipher`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE CIPHER 'ECDHE-RSA-AES128-GCM-SHA256' PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_require_ssl`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE SSL PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_require_x509`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE X509 PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_resource_full`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE WITH MAX_QUERIES_PER_HOUR 5000 MAX_UPDATES_PER_HOUR 500 MAX_CONNECTIONS_PER_HOUR 100 MAX_USER_CONNECTIONS 25 PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_role*/starslash`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_role_a`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_role_b`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_role_c`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_role_d`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_role_dflt_one`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_role_dflt_two`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_role_dflt_x`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_role_dflt_y`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_role_mandatory`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_role_static`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_routine_admin`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_shared_role`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_shared_user_1`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_shared_user_2`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_shared_user_3`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_shared_user_4`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_shared_user_5`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_simple`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_starslash_comment`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: account option dropped (no MariaDB equivalent): ATTRIBUTE '{"comment": "has */ in body"}'
CREATE USER `src_starslash_escaped`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: account option dropped (no MariaDB equivalent): ATTRIBUTE '{"comment": "has \\\\*/ escaped"}'
CREATE USER `src_static_dyn_mix`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_static_priv`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_static_role_mix`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_system_grants`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_unlock`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_usage_only`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_with_attribute`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: account option dropped (no MariaDB equivalent): ATTRIBUTE '{"team": "db_migration", "contact": "alice@example.com"}'
CREATE USER `src_with_comment`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: account option dropped (no MariaDB equivalent): ATTRIBUTE '{"comment": "Migration test user with embedded \'quote\' character"}'
CREATE USER `src_用户`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_edge_wildcard_host`@`192.168.%.%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_role_dual`@`h1` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER `src_role_dual`@`h2` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE ACCOUNT LOCK;
CREATE USER ``@`localhost` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `mysql.infoschema`@`localhost` IDENTIFIED WITH 'caching_sha2_password' AS '$A$005$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;
CREATE USER `mysql.session`@`localhost` IDENTIFIED WITH 'caching_sha2_password' AS '$A$005$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;
CREATE USER `mysql.sys`@`localhost` IDENTIFIED WITH 'caching_sha2_password' AS '$A$005$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;
CREATE USER `root`@`localhost` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_host_dual`@`localhost` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `src_host_localhost`@`localhost` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$abcdefghijklmnopqrstABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
SELECT COALESCE(CURRENT_ROLE(),'NONE') into @current_role;
CREATE ROLE IF NOT EXISTS mariadb_dump_import_role;
GRANT mariadb_dump_import_role TO CURRENT_USER();
SET ROLE mariadb_dump_import_role;
/*M!100005 SET @mariadb_dump_saved_sql_mode=@@sql_mode,
  sql_mode=CONCAT_WS(',', NULLIF(@@sql_mode, ''), 'NO_AUTO_CREATE_USER') */;
/*M!100005 CREATE ROLE 'Public_0001' WITH ADMIN mariadb_dump_import_role */;
# NOTE: MySQL role 'Public'@'%' is the MariaDB role 'Public_0001'
/*M!100005 CREATE ROLE 'Public_0001_0002' WITH ADMIN mariadb_dump_import_role */;
# NOTE: MySQL role 'Public_0001'@'%' is the MariaDB role 'Public_0001_0002'
/*M!100005 CREATE ROLE 'none_0003' WITH ADMIN mariadb_dump_import_role */;
# NOTE: MySQL role 'none'@'%' is the MariaDB role 'none_0003'
/*M!100005 CREATE ROLE 'src_default_a' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'src_default_b' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'src_hybrid' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'src_role_a' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'src_role_b' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'src_role_c' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'src_role_d' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'src_role_dflt_one' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'src_role_dflt_two' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'src_role_dflt_x' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'src_role_dflt_y' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'src_role_dual_0004' WITH ADMIN mariadb_dump_import_role */;
# NOTE: MySQL role 'src_role_dual'@'h1' is the MariaDB role 'src_role_dual_0004'
/*M!100005 CREATE ROLE 'src_role_dual_0005' WITH ADMIN mariadb_dump_import_role */;
# NOTE: MySQL role 'src_role_dual'@'h2' is the MariaDB role 'src_role_dual_0005'
/*M!100005 CREATE ROLE 'src_role_mandatory' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'src_shared_role' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 GRANT 'Public_0001' TO 'src_name_member'@'%' */;
/*M!100005 GRANT 'Public_0001_0002' TO 'src_name_member'@'%' */;
/*M!100005 GRANT 'none_0003' TO 'src_name_member'@'%' */;
/*M!100005 GRANT 'src_default_a' TO 'src_combined'@'%' */;
/*M!100005 GRANT 'src_default_a' TO 'src_default_role_many'@'%' */;
/*M!100005 GRANT 'src_default_a' TO 'src_default_role_one'@'%' */;
/*M!100005 GRANT 'src_default_a' TO 'src_shared_user_1'@'%' */;
/*M!100005 GRANT 'src_default_a' TO 'src_shared_user_2'@'%' */;
/*M!100005 GRANT 'src_default_b' TO 'src_default_role_many'@'%' */;
/*M!100005 GRANT 'src_hybrid' TO 'src_hybrid_member'@'%' */;
/*M!100005 GRANT 'src_role_a' TO 'src_admin_chain_root'@'%' WITH ADMIN OPTION */;
/*M!100005 GRANT 'src_role_a' TO 'src_simple'@'%' WITH ADMIN OPTION */;
/*M!100005 GRANT 'src_role_b' TO 'src_role_a' WITH ADMIN OPTION */;
/*M!100005 GRANT 'src_role_c' TO 'src_hybrid' */;
/*M!100005 GRANT 'src_role_c' TO 'src_hybrid'@'%' */;
# WARNING: role src_role_c@% is granted to src_orphan_member@%, and one of them is not in mysql.user; the grant is skipped
/*M!100005 GRANT 'src_role_c' TO 'src_role_b' WITH ADMIN OPTION */;
/*M!100005 GRANT 'src_role_d' TO 'src_simple'@'%' */;
/*M!100005 GRANT 'src_role_dflt_one' TO 'src_dflt_member'@'%' */;
/*M!100005 GRANT 'src_role_dflt_two' TO 'src_dflt_member'@'%' */;
/*M!100005 GRANT 'src_role_dflt_x' TO 'src_role_dflt_one' */;
/*M!100005 GRANT 'src_role_dflt_x' TO 'src_role_dflt_two' */;
/*M!100005 GRANT 'src_role_dflt_y' TO 'src_role_dflt_two' */;
/*M!100005 GRANT 'src_role_dual_0004' TO 'src_dual_member'@'%' */;
/*M!100005 GRANT 'src_role_dual_0005' TO 'src_dual_member'@'%' */;
/*M!100005 GRANT 'src_shared_role' TO 'src_shared_user_1'@'%' */;
/*M!100005 GRANT 'src_shared_role' TO 'src_shared_user_2'@'%' WITH ADMIN OPTION */;
/*M!100005 GRANT 'src_shared_role' TO 'src_shared_user_3'@'%' */;
/*M!100005 GRANT 'src_shared_role' TO 'src_shared_user_4'@'%' */;
/*M!100005 GRANT 'src_shared_role' TO 'src_shared_user_5'@'%' WITH ADMIN OPTION */;
/*M!101100 GRANT 'src_role_mandatory' TO PUBLIC */;
# WARNING: mandatory role 'src_role_mandatory'@'%' is granted to PUBLIC: it is active for every account, on MySQL only where it is enabled
/*M!100005 SET DEFAULT ROLE 'src_default_a' FOR 'src_combined'@'%' */;
/*M!100005 CREATE ROLE 'src_default_role_many_default_roles' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 GRANT 'src_default_a' TO 'src_default_role_many_default_roles' */;
/*M!100005 GRANT 'src_default_b' TO 'src_default_role_many_default_roles' */;
/*M!100005 GRANT 'src_default_role_many_default_roles' TO 'src_default_role_many'@'%' */;
/*M!100005 SET DEFAULT ROLE 'src_default_role_many_default_roles' FOR 'src_default_role_many'@'%' */;
/*M!100005 SET DEFAULT ROLE 'src_default_a' FOR 'src_default_role_one'@'%' */;
# NOTE: the default roles of the locked role src_role_dflt_one@% are not dumped
# NOTE: the default roles of the locked role src_role_dflt_two@% are not dumped
GRANT USAGE ON *.* TO ``@`%`;
GRANT USAGE ON *.* TO `O'Brien`@`%`;
GRANT SELECT ON `src_db`.* TO `O'Brien`@`%`;
GRANT USAGE ON *.* TO `src edge space`@`%`;
GRANT SELECT ON `src_db`.* TO `src edge space`@`%`;
GRANT USAGE ON *.* TO `src_admin_chain_root`@`%`;
# WARNING: dropped MySQL privilege(s) CREATE ROLE, DROP ROLE (no MariaDB equivalent): ON *.* TO `src_all_privs`@`%` WITH GRANT OPTION
/*M!110400 GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, RELOAD, SHUTDOWN, PROCESS, FILE, REFERENCES, INDEX, ALTER, SHOW DATABASES, SUPER, CREATE TEMPORARY TABLES, LOCK TABLES, EXECUTE, REPLICATION SLAVE, REPLICATION CLIENT, CREATE VIEW, SHOW VIEW, CREATE ROUTINE, ALTER ROUTINE, CREATE USER, EVENT, TRIGGER, CREATE TABLESPACE ON *.* TO `src_all_privs`@`%` WITH GRANT OPTION */;
# WARNING: dropped MySQL privilege(s) ALLOW_NONEXISTENT_DEFINER, APPLICATION_PASSWORD_ADMIN, AUDIT_ABORT_EXEMPT, AUDIT_ADMIN, AUTHENTICATION_POLICY_ADMIN, BACKUP_ADMIN, BINLOG_ENCRYPTION_ADMIN, CLONE_ADMIN, CREATE_SPATIAL_REFERENCE_SYSTEM, ENCRYPTION_KEY_ADMIN, FIREWALL_EXEMPT, FLUSH_OPTIMIZER_COSTS, FLUSH_PRIVILEGES, FLUSH_STATUS, FLUSH_TABLES, FLUSH_USER_RESOURCES, GROUP_REPLICATION_ADMIN, GROUP_REPLICATION_STREAM, INNODB_REDO_LOG_ARCHIVE, INNODB_REDO_LOG_ENABLE, MANAGE_DATA_MASKING_POLICY, OPTIMIZE_LOCAL_TABLE, PASSWORDLESS_USER_ADMIN, PERSIST_RO_VARIABLES_ADMIN, RESOURCE_GROUP_ADMIN, RESOURCE_GROUP_USER, ROLE_ADMIN, SENSITIVE_VARIABLES_OBSERVER, SERVICE_CONNECTION_ADMIN, SESSION_VARIABLES_ADMIN, SYSTEM_USER, SYSTEM_VARIABLES_ADMIN, TABLE_ENCRYPTION_ADMIN, TELEMETRY_LOG_ADMIN, TRANSACTION_GTID_TAG, XA_RECOVER_ADMIN (no MariaDB equivalent): ON *.* TO `src_all_privs`@`%` WITH GRANT OPTION
# WARNING: WITH GRANT OPTION dropped from a translated privilege: MariaDB would make all global privileges of the account grantable: ON *.* TO `src_all_privs`@`%` WITH GRANT OPTION
/*M!110400 GRANT BINLOG REPLAY, BINLOG ADMIN, CONNECTION ADMIN, REPLICATION SLAVE ADMIN, SET USER, SHOW CREATE ROUTINE ON *.* TO `src_all_privs`@`%` */;
GRANT USAGE ON *.* TO `src_auth_caching_sha2`@`%`;
GRANT USAGE ON *.* TO `src_auth_no_pwd`@`%`;
GRANT SELECT ON `src_db`.* TO `src_auth_no_pwd`@`%`;
GRANT USAGE ON *.* TO `src_auth_sha256`@`%`;
GRANT USAGE ON *.* TO `src_col_priv`@`%`;
GRANT SELECT (`col_a`, `col_underscore_b`), UPDATE (`col_a`) ON `src_db`.`tab` TO `src_col_priv`@`%`;
GRANT USAGE ON *.* TO `src_combined`@`%`;
GRANT SELECT ON `src_db`.* TO `src_combined`@`%`;
GRANT USAGE ON *.* TO `src_db_underscore`@`%`;
GRANT SELECT ON `src_under_score_db`.* TO `src_db_underscore`@`%`;
GRANT USAGE ON *.* TO `src_default_role_many`@`%`;
GRANT USAGE ON *.* TO `src_default_role_one`@`%`;
GRANT USAGE ON *.* TO `src_dflt_member`@`%`;
GRANT USAGE ON *.* TO `src_dual_member`@`%`;
GRANT SELECT, RELOAD, PROCESS, SHOW DATABASES, LOCK TABLES, REPLICATION CLIENT, SHOW VIEW, CREATE USER, EVENT, TRIGGER ON *.* TO `src_dumper`@`%`;
GRANT SELECT ON `mysql`.* TO `src_dumper`@`%`;
GRANT USAGE ON *.* TO `src_dyn_all_dropped`@`%`;
# WARNING: dropped MySQL privilege(s) AUDIT_ADMIN, BACKUP_ADMIN, CLONE_ADMIN (no MariaDB equivalent): ON *.* TO `src_dyn_all_dropped`@`%`
GRANT USAGE ON *.* TO `src_dyn_flush_collapse`@`%`;
# WARNING: dropped MySQL privilege(s) FLUSH_OPTIMIZER_COSTS, FLUSH_STATUS, FLUSH_TABLES, FLUSH_USER_RESOURCES (no MariaDB equivalent): ON *.* TO `src_dyn_flush_collapse`@`%`
GRANT USAGE ON *.* TO `src_dyn_grant_opt`@`%`;
# WARNING: WITH GRANT OPTION dropped from a translated privilege: MariaDB would make all global privileges of the account grantable: ON *.* TO `src_dyn_grant_opt`@`%` WITH GRANT OPTION
/*M!110400 GRANT BINLOG REPLAY, BINLOG ADMIN, CONNECTION ADMIN ON *.* TO `src_dyn_grant_opt`@`%` */;
GRANT USAGE ON *.* TO `src_dyn_mapped`@`%`;
/*M!110400 GRANT BINLOG REPLAY, BINLOG ADMIN ON *.* TO `src_dyn_mapped`@`%` */;
GRANT USAGE ON *.* TO `src_dyn_mixed`@`%`;
# WARNING: dropped MySQL privilege(s) AUDIT_ADMIN, BACKUP_ADMIN (no MariaDB equivalent): ON *.* TO `src_dyn_mixed`@`%`
/*M!110400 GRANT BINLOG REPLAY, BINLOG ADMIN, CONNECTION ADMIN, SHOW CREATE ROUTINE ON *.* TO `src_dyn_mixed`@`%` */;
GRANT USAGE ON *.* TO `src_dyn_unmapped`@`%`;
# WARNING: dropped MySQL privilege(s) BACKUP_ADMIN (no MariaDB equivalent): ON *.* TO `src_dyn_unmapped`@`%`
GRANT USAGE ON *.* TO `src_edge@in_name`@`%`;
GRANT SELECT ON `src_db`.* TO `src_edge@in_name`@`%`;
GRANT USAGE ON *.* TO `src_failed_login`@`%`;
GRANT USAGE ON *.* TO `src_func_exec`@`%`;
GRANT EXECUTE ON FUNCTION `src_db`.`func_one` TO `src_func_exec`@`%`;
GRANT USAGE ON *.* TO `src_host_dual`@`%`;
GRANT SELECT ON `src_db`.`tab2` TO `src_host_dual`@`%`;
GRANT USAGE ON *.* TO `src_hybrid`@`%`;
GRANT SELECT ON `src_db`.`tab` TO `src_hybrid`@`%`;
GRANT USAGE ON *.* TO `src_hybrid_member`@`%`;
GRANT USAGE ON *.* TO `src_lock`@`%`;
GRANT USAGE ON *.* TO `src_max_conn`@`%`;
GRANT PROCESS ON *.* TO `src_multiscope`@`%`;
GRANT SELECT ON `src_db`.* TO `src_multiscope`@`%`;
GRANT INSERT, UPDATE ON `src_db`.`tab` TO `src_multiscope`@`%`;
GRANT USAGE ON *.* TO `src_name_member`@`%`;
# WARNING: WITH GRANT OPTION dropped: MariaDB would let the account grant its translated privileges, which MySQL did not: ON *.* TO `src_opt_static_dyn`@`%` WITH GRANT OPTION
/*M!110400 GRANT SELECT ON *.* TO `src_opt_static_dyn`@`%` */;
/*M!110400 GRANT CONNECTION ADMIN ON *.* TO `src_opt_static_dyn`@`%` */;
GRANT SELECT ON *.* TO `src_opt_static_dyn_opt`@`%` WITH GRANT OPTION;
# WARNING: WITH GRANT OPTION dropped from a translated privilege: MariaDB would make all global privileges of the account grantable: ON *.* TO `src_opt_static_dyn_opt`@`%` WITH GRANT OPTION
/*M!110400 GRANT CONNECTION ADMIN ON *.* TO `src_opt_static_dyn_opt`@`%` */;
GRANT SELECT, INSERT ON *.* TO `src_partial_revoke`@`%`;
# WARNING: a partial REVOKE is a DENY, which needs MariaDB 13.1.1: an older server does not run it, and the account is wider than on the MySQL source: ON `src_db`.* FROM `src_partial_revoke`@`%`
/*M!130101 DENY INSERT ON `src_db`.* TO `src_partial_revoke`@`%` */;
GRANT USAGE ON *.* TO `src_proc_exec`@`%`;
GRANT EXECUTE ON PROCEDURE `src_db`.`proc_one` TO `src_proc_exec`@`%`;
GRANT USAGE ON *.* TO `src_proxy_target`@`%`;
GRANT USAGE ON *.* TO `src_proxy_user`@`%`;
GRANT PROXY ON `src_proxy_target`@`%` TO `src_proxy_user`@`%`;
GRANT USAGE ON *.* TO `src_pwd_expire_default`@`%`;
GRANT USAGE ON *.* TO `src_pwd_expire_interval`@`%`;
GRANT USAGE ON *.* TO `src_pwd_expire_never`@`%`;
GRANT USAGE ON *.* TO `src_pwd_expire_now`@`%`;
GRANT USAGE ON *.* TO `src_pwd_history`@`%`;
GRANT USAGE ON *.* TO `src_pwd_require_current`@`%`;
GRANT USAGE ON *.* TO `src_pwd_reuse`@`%`;
GRANT USAGE ON *.* TO `src_require_cipher`@`%`;
GRANT USAGE ON *.* TO `src_require_ssl`@`%`;
GRANT USAGE ON *.* TO `src_require_x509`@`%`;
GRANT USAGE ON *.* TO `src_resource_full`@`%`;
GRANT USAGE ON *.* TO `src_role*/starslash`@`%`;
# WARNING: dropped MySQL privilege(s) BACKUP_ADMIN (no MariaDB equivalent): ON *.* TO `src_role*/starslash`@`%`
# WARNING: dropped MySQL privilege(s) CREATE ROLE, DROP ROLE (no MariaDB equivalent): ON *.* TO `src_role_static`@`%`
GRANT USAGE ON *.* TO `src_routine_admin`@`%`;
GRANT EXECUTE, CREATE ROUTINE, ALTER ROUTINE ON `src_db`.* TO `src_routine_admin`@`%`;
GRANT USAGE ON *.* TO `src_shared_user_1`@`%`;
GRANT USAGE ON *.* TO `src_shared_user_2`@`%`;
GRANT USAGE ON *.* TO `src_shared_user_3`@`%`;
GRANT USAGE ON *.* TO `src_shared_user_4`@`%`;
GRANT USAGE ON *.* TO `src_shared_user_5`@`%`;
GRANT USAGE ON *.* TO `src_simple`@`%`;
GRANT SELECT ON `src_db`.* TO `src_simple`@`%`;
GRANT USAGE ON *.* TO `src_starslash_comment`@`%`;
GRANT USAGE ON *.* TO `src_starslash_escaped`@`%`;
GRANT SELECT ON *.* TO `src_static_dyn_mix`@`%`;
# WARNING: dropped MySQL privilege(s) BACKUP_ADMIN (no MariaDB equivalent): ON *.* TO `src_static_dyn_mix`@`%`
/*M!110400 GRANT BINLOG REPLAY, BINLOG ADMIN ON *.* TO `src_static_dyn_mix`@`%` */;
GRANT RELOAD, PROCESS ON *.* TO `src_static_priv`@`%`;
GRANT SELECT, INSERT, UPDATE, DELETE ON `src_db`.* TO `src_static_priv`@`%`;
# WARNING: dropped MySQL privilege(s) CREATE ROLE, DROP ROLE (no MariaDB equivalent): ON *.* TO `src_static_role_mix`@`%`
/*M!110400 GRANT SELECT, INSERT ON *.* TO `src_static_role_mix`@`%` */;
GRANT USAGE ON *.* TO `src_system_grants`@`%`;
GRANT SELECT ON `performance_schema`.* TO `src_system_grants`@`%`;
GRANT SELECT ON `mysql`.`user` TO `src_system_grants`@`%`;
GRANT USAGE ON *.* TO `src_unlock`@`%`;
GRANT USAGE ON *.* TO `src_usage_only`@`%`;
GRANT USAGE ON *.* TO `src_with_attribute`@`%`;
GRANT USAGE ON *.* TO `src_with_comment`@`%`;
GRANT USAGE ON *.* TO `src_用户`@`%`;
GRANT SELECT ON `src_db`.* TO `src_用户`@`%`;
GRANT USAGE ON *.* TO `src_edge_wildcard_host`@`192.168.%.%`;
GRANT SELECT ON `src_db`.* TO `src_edge_wildcard_host`@`192.168.%.%`;
GRANT USAGE ON *.* TO ``@`localhost`;
GRANT SELECT ON *.* TO `mysql.infoschema`@`localhost`;
# WARNING: dropped MySQL privilege(s) AUDIT_ABORT_EXEMPT, FIREWALL_EXEMPT, SYSTEM_USER (no MariaDB equivalent): ON *.* TO `mysql.infoschema`@`localhost`
GRANT SHUTDOWN, SUPER ON *.* TO `mysql.session`@`localhost`;
# WARNING: dropped MySQL privilege(s) AUDIT_ABORT_EXEMPT, AUTHENTICATION_POLICY_ADMIN, BACKUP_ADMIN, CLONE_ADMIN, FIREWALL_EXEMPT, PERSIST_RO_VARIABLES_ADMIN, SESSION_VARIABLES_ADMIN, SYSTEM_USER, SYSTEM_VARIABLES_ADMIN (no MariaDB equivalent): ON *.* TO `mysql.session`@`localhost`
/*M!110400 GRANT CONNECTION ADMIN ON *.* TO `mysql.session`@`localhost` */;
GRANT SELECT ON `performance_schema`.* TO `mysql.session`@`localhost`;
GRANT SELECT ON `mysql`.`user` TO `mysql.session`@`localhost`;
GRANT USAGE ON *.* TO `mysql.sys`@`localhost`;
# WARNING: dropped MySQL privilege(s) AUDIT_ABORT_EXEMPT, FIREWALL_EXEMPT, SYSTEM_USER (no MariaDB equivalent): ON *.* TO `mysql.sys`@`localhost`
GRANT TRIGGER ON `sys`.* TO `mysql.sys`@`localhost`;
GRANT SELECT ON `sys`.`sys_config` TO `mysql.sys`@`localhost`;
# WARNING: dropped MySQL privilege(s) CREATE ROLE, DROP ROLE (no MariaDB equivalent): ON *.* TO `root`@`localhost` WITH GRANT OPTION
/*M!110400 GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, RELOAD, SHUTDOWN, PROCESS, FILE, REFERENCES, INDEX, ALTER, SHOW DATABASES, SUPER, CREATE TEMPORARY TABLES, LOCK TABLES, EXECUTE, REPLICATION SLAVE, REPLICATION CLIENT, CREATE VIEW, SHOW VIEW, CREATE ROUTINE, ALTER ROUTINE, CREATE USER, EVENT, TRIGGER, CREATE TABLESPACE ON *.* TO `root`@`localhost` WITH GRANT OPTION */;
# WARNING: dropped MySQL privilege(s) ALLOW_NONEXISTENT_DEFINER, APPLICATION_PASSWORD_ADMIN, AUDIT_ABORT_EXEMPT, AUDIT_ADMIN, AUTHENTICATION_POLICY_ADMIN, BACKUP_ADMIN, BINLOG_ENCRYPTION_ADMIN, CLONE_ADMIN, CREATE_SPATIAL_REFERENCE_SYSTEM, ENCRYPTION_KEY_ADMIN, FIREWALL_EXEMPT, FLUSH_OPTIMIZER_COSTS, FLUSH_PRIVILEGES, FLUSH_STATUS, FLUSH_TABLES, FLUSH_USER_RESOURCES, GROUP_REPLICATION_ADMIN, GROUP_REPLICATION_STREAM, INNODB_REDO_LOG_ARCHIVE, INNODB_REDO_LOG_ENABLE, MANAGE_DATA_MASKING_POLICY, OPTIMIZE_LOCAL_TABLE, PASSWORDLESS_USER_ADMIN, PERSIST_RO_VARIABLES_ADMIN, RESOURCE_GROUP_ADMIN, RESOURCE_GROUP_USER, ROLE_ADMIN, SENSITIVE_VARIABLES_OBSERVER, SERVICE_CONNECTION_ADMIN, SESSION_VARIABLES_ADMIN, SYSTEM_USER, SYSTEM_VARIABLES_ADMIN, TABLE_ENCRYPTION_ADMIN, TELEMETRY_LOG_ADMIN, TRANSACTION_GTID_TAG, XA_RECOVER_ADMIN (no MariaDB equivalent): ON *.* TO `root`@`localhost` WITH GRANT OPTION
# WARNING: WITH GRANT OPTION dropped from a translated privilege: MariaDB would make all global privileges of the account grantable: ON *.* TO `root`@`localhost` WITH GRANT OPTION
/*M!110400 GRANT BINLOG REPLAY, BINLOG ADMIN, CONNECTION ADMIN, REPLICATION SLAVE ADMIN, SET USER, SHOW CREATE ROUTINE ON *.* TO `root`@`localhost` */;
GRANT PROXY ON ``@`` TO `root`@`localhost` WITH GRANT OPTION;
GRANT USAGE ON *.* TO `src_host_dual`@`localhost`;
GRANT SELECT ON `src_db`.* TO `src_host_dual`@`localhost`;
GRANT USAGE ON *.* TO `src_host_localhost`@`localhost`;
GRANT SELECT ON `src_db`.* TO `src_host_localhost`@`localhost`;
/*M!110400 GRANT USAGE ON *.* TO 'Public_0001' */;
/*M!110400 GRANT INSERT ON `src_db`.`tab2` TO 'Public_0001' */;
/*M!110400 GRANT USAGE ON *.* TO 'Public_0001_0002' */;
/*M!110400 GRANT DELETE ON `src_db`.`tab2` TO 'Public_0001_0002' */;
/*M!110400 GRANT USAGE ON *.* TO 'none_0003' */;
/*M!110400 GRANT UPDATE ON `src_db`.`tab2` TO 'none_0003' */;
/*M!110400 GRANT USAGE ON *.* TO 'src_default_a' */;
/*M!110400 GRANT USAGE ON *.* TO 'src_default_b' */;
/*M!110400 GRANT USAGE ON *.* TO 'src_hybrid' */;
/*M!110400 GRANT SELECT ON `src_db`.`tab` TO 'src_hybrid' */;
/*M!110400 GRANT USAGE ON *.* TO 'src_role_a' */;
/*M!110400 GRANT SELECT ON `src_db`.* TO 'src_role_a' */;
/*M!110400 GRANT USAGE ON *.* TO 'src_role_b' */;
/*M!110400 GRANT INSERT ON `src_db`.* TO 'src_role_b' */;
/*M!110400 GRANT USAGE ON *.* TO 'src_role_c' */;
/*M!110400 GRANT UPDATE ON `src_db`.* TO 'src_role_c' */;
/*M!110400 GRANT SHOW DATABASES ON *.* TO 'src_role_d' */;
/*M!110400 GRANT USAGE ON *.* TO 'src_role_dflt_one' */;
/*M!110400 GRANT USAGE ON *.* TO 'src_role_dflt_two' */;
/*M!110400 GRANT USAGE ON *.* TO 'src_role_dflt_x' */;
/*M!110400 GRANT USAGE ON *.* TO 'src_role_dflt_y' */;
/*M!110400 GRANT USAGE ON *.* TO 'src_role_dual_0004' */;
/*M!110400 GRANT SELECT ON `src_db`.`tab` TO 'src_role_dual_0004' */;
/*M!110400 GRANT USAGE ON *.* TO 'src_role_dual_0005' */;
/*M!110400 GRANT INSERT ON `src_db`.`tab` TO 'src_role_dual_0005' */;
/*M!110400 GRANT USAGE ON *.* TO 'src_role_mandatory' */;
/*M!110400 GRANT USAGE ON *.* TO 'src_shared_role' */;
/*M!110400 GRANT SELECT ON `src_db`.`tab` TO 'src_shared_role' */;
/*M!110400 GRANT EXECUTE ON PROCEDURE `src_db`.`proc_one` TO 'src_shared_role' */;
/*M!100005 SET sql_mode=@mariadb_dump_saved_sql_mode */;
SET ROLE NONE;
DROP ROLE mariadb_dump_import_role;
/*M!100203 EXECUTE IMMEDIATE CONCAT('SET ROLE ', @current_role) */;
SET TIME_ZONE=@OLD_TIME_ZONE;

SET SQL_MODE=@OLD_SQL_MODE;
SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS;
SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS;
SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT;
SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS;
SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION;
/*M!100616 SET NOTE_VERBOSITY=@OLD_NOTE_VERBOSITY */;

-- Dump completed on 2026-11-09 17:08:23
