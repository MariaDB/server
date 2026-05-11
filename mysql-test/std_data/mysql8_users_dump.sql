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
CREATE USER ``@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$FMZ_]I-tzZU`aByEVhT9V.2uIloqRkcMJ2PLmibTmHlCw.WJKzP1d0XIsUyC' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `O'Brien`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$\nr]azkIep0uR[4M4sWmbRdn4cGlrRQfNrec/vBIzJN9PZr1j0kSlYFa6n9' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8 edge space`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$\\0\"Ge}&C[nC3F)LcO9zzpJA8bRXRYBBeXyWW8Uf2gJkPLkcKNCMRTSCN2' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_all_privs`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$sm{lZs z^7O(;p;I0xEPdBny1g5uL23Dvq72X8mb/SR6DPKvrO70zAtM7r4' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_auth_caching_sha2`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$	bK#i8E,\\\n~F8+8cnnN53cmw8R1WcCV.P.tgX88EPff9bLmFmZAmeQdE1E.' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_auth_no_pwd`@`%` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_auth_sha256`@`%` REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;
# WARNING: auth plugin 'sha256_password' not shipped with MariaDB; user created with no password and ACCOUNT LOCK - set a password and ACCOUNT UNLOCK before login
CREATE USER `m8_col_priv`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$ki#x2L1E;rLI\Z+O=e15MzJ0iz872j3s0jdbKLAgjLlqkrH.ecWWVVBXfsr6' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_combined`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A${c2PCf~,XEgD>t6X3wiSe61Dyv61/fZNbBPcnIjggVlbJQb5lRMFq3QkXD' REQUIRE SSL WITH MAX_USER_CONNECTIONS 5 PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;
# WARNING: password policy clause dropped (no MariaDB equivalent): PASSWORD HISTORY 3
# WARNING: password policy clause dropped (no MariaDB equivalent): PASSWORD REUSE INTERVAL 90 DAY
CREATE USER `m8_db_underscore`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$-A.N\nR9l2/##X59^4OpQdgFtCcPrmDkfWJBStpsm9vreIQGrwrBkxBnaLlP1' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_default_role_many`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$ff4>%G\ZD3HlJWR-yH4yQR.CnWQV8R79qyZr0xqjbOgQVjZcgrJGCFoauSd6' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_default_role_one`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$?0o0\\	m/YN~8YmDJsaqwLeA2YxTA/yTixyhgEwXTGWwZomyUBEzfQtK1' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_dual_member`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$WhDj\"9	Q|]=Z~gn0hU1l8HRvRaqpNso4AB4MSyznFLYw9Xgt5u7BGUWHi5' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_dumper`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$+Z#c8Exf%79UFZfK4WoaLF.lZnlJNz80pT57y.gvoq0rrZTZK2n9T/NWXmA' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_dyn_all_dropped`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$!o~2qV{<\\R7+iUmDv9u0c0q5sQwvTGusxqWyL5JTSkXwj5L84M9sq0IHi9' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_dyn_flush_collapse`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$d o7l#&VO{x`J!vywu9DM5pGXbuF3SxdnVvhdngbxX2EcNvilmSH3gxWPx0' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_dyn_grant_opt`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$.((\ZK:u<e0q9dDs:QHChMVxQpRRvGD7HSFLCqcIcAD03V3HLVAE.XKZRGr5' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_dyn_mapped`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$p@k+AU!&*=*0tvUdUCopD3fQ2tXBkD9Hp3iRdY.PMVFr6cWXbKj7WTe86dHW321' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_dyn_mixed`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$71r2SI&\nCs\\L115m&NChqNpnkdldmI/nE97F5dGrztgsA113RGIuCfkf1ZH2' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_dyn_unmapped`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$tc3\r\"fGW79\"oA7:rJuj/VNZ0Akrj4X7.YYoSPXNegGArpzFVGVqZz398u/' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_edge@in_name`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$6im%Mv97t;3aO\rTM/l97fzgpCbQj8je/abW65A66j8fzbyTNBP10e4QCD' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_failed_login`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$	%\n:	k!6:(JNjei!tlLjb8N1.b2ZLo/Ki7WZUl0JjgbBRezn73qm68NI71mk9' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: account option dropped (no MariaDB equivalent): FAILED_LOGIN_ATTEMPTS 3
# WARNING: account option dropped (no MariaDB equivalent): PASSWORD_LOCK_TIME 1
CREATE USER `m8_func_exec`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$@}]8H-)4oAeP[%X.XnuQaFYDDXuXTKfIVD/xbt3Ajakcz4CqCIopEDxeLW2' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_host_dual`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$O|]fSqUTj\r eze\"E\ZRmgsR3/Ew98bghKeyh0p/QrNlOsV3zsvBQ3LKNMcbH1' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_hybrid`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$ag8F\n3iFzGE`}@\rK.TLJxjQ2uVEOaXCfQJuqUJms2YPB/DSAkaHojkCCL6' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_hybrid_member`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$s)X\ZjKKj9Jlv)ey7x;6I2ucvDYyQveLLVtfjxvtItWZPMcKOYsFg4HFvTuRjD' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_lock`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$>4OwIJN2E;f`:sDlCu1rFclAFr2CVuRwmYsaREcvkFdm0U0frjtjxnz6a/D' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;
CREATE USER `m8_max_conn`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$hk8>W1LbDEg\nT~Oj8zc.LcdjiksX0UIsKeLA2Ci/Fn8jdvasTdxDPyWK7' REQUIRE NONE WITH MAX_QUERIES_PER_HOUR 1000 MAX_USER_CONNECTIONS 10 PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_multiscope`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$t|QRAI!/\rh!%M4yZ/6HEN7qH7BpaTl2yAGAPiqFXOchxXiMD5Z.5WED8' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_partial_revoke`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$>K)`\ZdZIL{VV?A6AJZD.n1YA4YWsCjbGrvkL/jxRC1UffRvb.9oxXleb0U4PXq5' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_proc_exec`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$%B=GRz|.3c6m{1hfSGCRKK9gnYCrbrDInH.Qqi5/hjuogTxWE7iPfpVCX6' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_proxy_target`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$()|t7(9=_M	Lt3h6XrPDiNrsiv.V5ZhJwD7GW8qL6Bd6t7gpnqcjDmhdeLM8' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_proxy_user`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$xcU>DBq@{3#v]SK*FBwCry4uUTT3aWvUGh.Xkj/EbuofXVQ23m7w1AoRV16' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_pwd_expire_default`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$z\rY7xW	>HI:%f]#JZn5aO8BLTjHvAaJnpiaxkHXEjUA0XFwEOLuEsjNHLD' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_pwd_expire_interval`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$v3/a)ypB#Jd41@)~f/sL6x3xF.tSokS5Wk9SVf9NZrBvwYH9VmyV7O4PJi3' REQUIRE NONE PASSWORD EXPIRE INTERVAL 90 DAY ACCOUNT UNLOCK;
CREATE USER `m8_pwd_expire_never`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$\"m\Z)-Z\r]jBy1@@0weKTln4AVlA5X72fk7jFwT9MDbbzvnf2W0LbiWUhO8' REQUIRE NONE PASSWORD EXPIRE NEVER ACCOUNT UNLOCK;
CREATE USER `m8_pwd_expire_now`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$~=sT((x%?^v[Y<zxDi4.LVnQODsa/orH4fSsPrD94uRM5tJLVaLWKBLa6' REQUIRE NONE PASSWORD EXPIRE ACCOUNT UNLOCK;
CREATE USER `m8_pwd_history`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$7Ck~VZZ{a%\\unbDSfSizjHdGUt3ab4k7achn7SOlAe/BxjY4Buxby/' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: password policy clause dropped (no MariaDB equivalent): PASSWORD HISTORY 5
CREATE USER `m8_pwd_require_current`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$9uc@H;Ns(LHfB>r99VPB6ONFNBEuO4lLmTmRuJuVD5aE/HF49/GP9m.7C7B' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: password policy clause dropped (no MariaDB equivalent): PASSWORD REQUIRE CURRENT
CREATE USER `m8_pwd_reuse`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$&Rn2+\rj-E&(U!i*)RAkCw22jcEIKbS63YXhdcL00GTGktWnbnwMWG7Ieq7wy1' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: password policy clause dropped (no MariaDB equivalent): PASSWORD REUSE INTERVAL 365 DAY
CREATE USER `m8_require_cipher`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$Bp^`;g>Zr.iDGN%a41CUDgnyFyE08rgGtPLRpP/Ww7AYKQptoh8EA2vSQvz5' REQUIRE CIPHER 'ECDHE-RSA-AES128-GCM-SHA256' PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_require_ssl`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$:a*b:3D{U%D&`0&4we2mV10v0AVS3JsiCX2XmjnWGoioUH7WMlesx2n3q6' REQUIRE SSL PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_require_x509`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$\"C7>ju	\rkm^Kuas/xnCJOdRCSFKowK9vpHmLhltD358ekbJaGQUeBvz1.D' REQUIRE X509 PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_resource_full`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$CB!+FMRX|(3\\p\'&HnhC4rx4H2RIOkKgTRseBY05kdzXJ4eCU.JGEqUvPU/' REQUIRE NONE WITH MAX_QUERIES_PER_HOUR 5000 MAX_UPDATES_PER_HOUR 500 MAX_CONNECTIONS_PER_HOUR 100 MAX_USER_CONNECTIONS 25 PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_role_static`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$|p>j!V3Nhdo}ef\\tFjQ3RhbYSwDSl1F6iflWCwrF5yuQJkF33FT8yfsJ1A' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_routine_admin`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$J{9ndjP%,Cl:Z&6dN4M1T/hkxnyUVYMXVKzbidJr1sXNsDaU1R9ctxt84mpA' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_shared_user_1`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$MxoQ1k66olY\nQ\nS\Z0Rjk4mHBaJst.0LilpIcQPRCITyodrcyx2Yp/wpfBub6' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_shared_user_2`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$^c1,6779\"7?PN+~j:jAfMmIQonXwgSbmY4ywjy.yilJPZeO5FN/kQj0TAn71' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_shared_user_3`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$Q7v0M#KW{rL\'1>YSvdrvl8fT7pkQvl8/zzoZi6gel16N5jBJ1OQjMIcEA17' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_shared_user_4`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$Z%P#Ie0)}%(F|	eFHjalj9Iyl/zVROajvKNWvm.hV.cLqhDBw.fqKV9kkC' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_shared_user_5`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$!smtjJTSp8Nn/VLtR.bfPuLz0EqdTJNVrXcS14HLrOWjs0i3JPY8kX.8' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_simple`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$hE%IhaKxmz(yiB\rM3Y1nNQqNKeHJy8Ddq7OkdaRZloD/QeK4HnwwZWpHtV/A' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_starslash_comment`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$lX,\" r-?)SP%ft<KJUCr8LlLTkCRxQpNHhJ8opVmYbIr7oFytz09/v5WsufKd6D' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: account option dropped (no MariaDB equivalent): ATTRIBUTE '{"comment": "has */ in body"}'
CREATE USER `m8_starslash_escaped`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$66|codaYU(l6%\\IfnBTSx706tDilo.Jvn5WmIuk6N0DgvZg99FlW8WLsB' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: account option dropped (no MariaDB equivalent): ATTRIBUTE '{"comment": "has \\\\*/ escaped"}'
CREATE USER `m8_static_dyn_mix`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$Tqk\Z/O/;W4Hc5%-6 C2rFSbGeKkVKJEIuwxq1YvwmhJ3/h0kPVMBQ9qxEOSA' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_static_priv`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$\rQp]#W`4G]W\rDiBFkUgR0Mc4eAL0yct4n9Sn/TMhkeockulHSGrub./VL2' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_static_role_mix`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$B!lF1.EiZ7b`Kq1qT9GfH6WKuR5rHaGsyDOF5.aaXqo8s0hQ5u5g.VYxFHG0' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_system_grants`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$f^!!Kg\n\'jF]STki*9QZCfgOrAsTexBsJRAgzAe9WFpTJk//Q7Wnqt0ST3h8' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_unlock`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$}4yUuc:I]?mpo:F8S75fUwwwHk2pK6jJ.DSO69e.1kLVSheAm.yEi04W4' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_usage_only`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$1Njb\Z\Z:C%aNl::xX1kqOziTMqFcv8/UJxFW7B56gJONavujWd258hsTaB' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_with_attribute`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$\rmnC\\%|<dz5\\f .aR/UrmzYP1oavLKzB8gYZXHCMvJ02O8q0Q951JMGea6' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: account option dropped (no MariaDB equivalent): ATTRIBUTE '{"team": "db_migration", "contact": "alice@example.com"}'
CREATE USER `m8_with_comment`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$6%7GnA!,U2QN	U7msxvzSQAqDGquRIFdceT65rHPoEimCzBVG151tbX/5' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
# WARNING: account option dropped (no MariaDB equivalent): ATTRIBUTE '{"comment": "Migration test user with embedded \'quote\' character"}'
CREATE USER `m8_用户`@`%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$G7q[N8	Fo&=iR%/17cGeC.6z9lTnN2U3QbZZqyX7ro7G78G61KbaatQD.' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_edge_wildcard_host`@`192.168.%.%` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$~\rJS^(vjRB3!!SN1QzHwlbLrF5R4dWfO2/zKtZwXIaBXEIqHmJdZunR6' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER ``@`localhost` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$0)v@fc]_Ci\\_W\Z~%O_lmIYm90LWjFS4OpWE/XWlR7aHEpJHzRRg1zju4pS1i1' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_host_dual`@`localhost` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$Xf5C5g24,=\ZzqTMJQqND8nhIcQ5JGW7ClJ2tzl9tYw2SQgR1LEXwt6' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `m8_host_localhost`@`localhost` IDENTIFIED WITH 'caching_sha2_password' AS '$A$00A$dmSE}E/UZWOBHeeE9bEaxAi2Nl/gBeaMhYKVgp01nFxqLFulDnEal4wkvcB' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
CREATE USER `mysql.infoschema`@`localhost` IDENTIFIED WITH 'caching_sha2_password' AS '$A$005$THISISACOMBINATIONOFINVALIDSALTANDPASSWORDTHATMUSTNEVERBRBEUSED' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;
CREATE USER `mysql.session`@`localhost` IDENTIFIED WITH 'caching_sha2_password' AS '$A$005$THISISACOMBINATIONOFINVALIDSALTANDPASSWORDTHATMUSTNEVERBRBEUSED' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;
CREATE USER `mysql.sys`@`localhost` IDENTIFIED WITH 'caching_sha2_password' AS '$A$005$THISISACOMBINATIONOFINVALIDSALTANDPASSWORDTHATMUSTNEVERBRBEUSED' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;
CREATE USER `root`@`localhost` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;
SELECT COALESCE(CURRENT_ROLE(),'NONE') into @current_role;
CREATE ROLE IF NOT EXISTS mariadb_dump_import_role;
GRANT mariadb_dump_import_role TO CURRENT_USER();
SET ROLE mariadb_dump_import_role;
/*M!100005 SET @mariadb_dump_saved_sql_mode=@@sql_mode,
  sql_mode=CONCAT_WS(',', NULLIF(@@sql_mode, ''), 'NO_AUTO_CREATE_USER') */;
# WARNING: MySQL account 'm8_admin_chain_root' (hosts '%') is locked, passwordless, expired and granted to nobody; dumped as CREATE ROLE (MySQL 8 has no role flag), recreate it as a user if it was a disabled account
/*M!100005 CREATE ROLE 'm8_admin_chain_root' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'm8_default_a' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'm8_default_b' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'm8_hybrid' WITH ADMIN mariadb_dump_import_role */;
# WARNING: MySQL account 'm8_role*/starslash' (hosts '%') is locked, passwordless, expired and granted to nobody; dumped as CREATE ROLE (MySQL 8 has no role flag), recreate it as a user if it was a disabled account
/*M!100203 EXECUTE IMMEDIATE CONCAT('CREATE ROLE ''m8_role*', '/starslash'' WITH ADMIN mariadb_dump_import_role') */;
/*M!100005 CREATE ROLE 'm8_role_a' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'm8_role_b' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'm8_role_c' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'm8_role_d' WITH ADMIN mariadb_dump_import_role */;
# WARNING: 2 MySQL roles named 'm8_role_dual' on different hosts ('h1','h2') map onto one hostless MariaDB role; they are merged
/*M!100005 CREATE ROLE 'm8_role_dual' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 CREATE ROLE 'm8_shared_role' WITH ADMIN mariadb_dump_import_role */;
/*M!100005 GRANT 'm8_default_a' TO 'm8_combined'@'%' */;
/*M!100005 GRANT 'm8_default_a' TO 'm8_default_role_many'@'%' */;
/*M!100005 GRANT 'm8_default_a' TO 'm8_default_role_one'@'%' */;
/*M!100005 GRANT 'm8_default_a' TO 'm8_shared_user_1'@'%' */;
/*M!100005 GRANT 'm8_default_a' TO 'm8_shared_user_2'@'%' */;
/*M!100005 GRANT 'm8_default_b' TO 'm8_default_role_many'@'%' */;
/*M!100005 GRANT 'm8_hybrid' TO 'm8_hybrid_member'@'%' */;
/*M!100005 GRANT 'm8_role_a' TO 'm8_admin_chain_root' WITH ADMIN OPTION */;
/*M!100005 GRANT 'm8_role_a' TO 'm8_simple'@'%' WITH ADMIN OPTION */;
/*M!100005 GRANT 'm8_role_b' TO 'm8_role_a' WITH ADMIN OPTION */;
/*M!100005 GRANT 'm8_role_c' TO 'm8_hybrid' */;
/*M!100005 GRANT 'm8_role_c' TO 'm8_hybrid'@'%' */;
# WARNING: role 'm8_role_c' is granted to 'm8_orphan_member'@'%', which is not in mysql.user; the grant is skipped
/*M!100005 GRANT 'm8_role_c' TO 'm8_role_b' WITH ADMIN OPTION */;
/*M!100005 GRANT 'm8_role_d' TO 'm8_simple'@'%' */;
/*M!100005 GRANT 'm8_role_dual' TO 'm8_dual_member'@'%' */;
/*M!100005 GRANT 'm8_shared_role' TO 'm8_shared_user_1'@'%' */;
/*M!100005 GRANT 'm8_shared_role' TO 'm8_shared_user_2'@'%' WITH ADMIN OPTION */;
/*M!100005 GRANT 'm8_shared_role' TO 'm8_shared_user_3'@'%' */;
/*M!100005 GRANT 'm8_shared_role' TO 'm8_shared_user_4'@'%' */;
/*M!100005 GRANT 'm8_shared_role' TO 'm8_shared_user_5'@'%' WITH ADMIN OPTION */;
# WARNING: mandatory_roles is m8_role_c: these roles are granted to every user implicitly and MariaDB has no equivalent; grant them explicitly if needed
GRANT USAGE ON *.* TO ``@`%`;
GRANT USAGE ON *.* TO ``@`localhost`;
GRANT USAGE ON *.* TO `O'Brien`@`%`;
GRANT SELECT ON `m8_db`.* TO `O'Brien`@`%`;
GRANT USAGE ON *.* TO `m8 edge space`@`%`;
GRANT SELECT ON `m8_db`.* TO `m8 edge space`@`%`;
# WARNING: dropped MySQL privilege CREATE ROLE (no MariaDB equivalent)
# WARNING: dropped MySQL privilege DROP ROLE (no MariaDB equivalent)
/*M!100005 GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, RELOAD, SHUTDOWN, PROCESS, FILE, REFERENCES, INDEX, ALTER, SHOW DATABASES, SUPER, CREATE TEMPORARY TABLES, LOCK TABLES, EXECUTE, REPLICATION SLAVE, REPLICATION CLIENT, CREATE VIEW, SHOW VIEW, CREATE ROUTINE, ALTER ROUTINE, CREATE USER, EVENT, TRIGGER, CREATE TABLESPACE ON *.* TO `m8_all_privs`@`%` WITH GRANT OPTION */;
# WARNING: dropped MySQL privilege ALLOW_NONEXISTENT_DEFINER (no MariaDB equivalent)
# WARNING: dropped MySQL privilege APPLICATION_PASSWORD_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege AUDIT_ABORT_EXEMPT (no MariaDB equivalent)
# WARNING: dropped MySQL privilege AUDIT_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege AUTHENTICATION_POLICY_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege BACKUP_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege BINLOG_ENCRYPTION_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege CLONE_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege CREATE_SPATIAL_REFERENCE_SYSTEM (no MariaDB equivalent)
# WARNING: dropped MySQL privilege ENCRYPTION_KEY_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FIREWALL_EXEMPT (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FLUSH_OPTIMIZER_COSTS (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FLUSH_PRIVILEGES (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FLUSH_STATUS (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FLUSH_TABLES (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FLUSH_USER_RESOURCES (no MariaDB equivalent)
# WARNING: dropped MySQL privilege GROUP_REPLICATION_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege GROUP_REPLICATION_STREAM (no MariaDB equivalent)
# WARNING: dropped MySQL privilege INNODB_REDO_LOG_ARCHIVE (no MariaDB equivalent)
# WARNING: dropped MySQL privilege INNODB_REDO_LOG_ENABLE (no MariaDB equivalent)
# WARNING: dropped MySQL privilege MANAGE_DATA_MASKING_POLICY (no MariaDB equivalent)
# WARNING: dropped MySQL privilege OPTIMIZE_LOCAL_TABLE (no MariaDB equivalent)
# WARNING: dropped MySQL privilege PASSWORDLESS_USER_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege PERSIST_RO_VARIABLES_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege RESOURCE_GROUP_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege RESOURCE_GROUP_USER (no MariaDB equivalent)
# WARNING: dropped MySQL privilege ROLE_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SENSITIVE_VARIABLES_OBSERVER (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SERVICE_CONNECTION_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SESSION_VARIABLES_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SYSTEM_USER (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SYSTEM_VARIABLES_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege TABLE_ENCRYPTION_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege TELEMETRY_LOG_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege TRANSACTION_GTID_TAG (no MariaDB equivalent)
# WARNING: dropped MySQL privilege XA_RECOVER_ADMIN (no MariaDB equivalent)
# WARNING: WITH GRANT OPTION dropped from a translated privilege: MariaDB would make all global privileges of the account grantable
/*M!100005 GRANT BINLOG REPLAY, BINLOG ADMIN, CONNECTION ADMIN, REPLICATION SLAVE ADMIN, SET USER, SHOW CREATE ROUTINE ON *.* TO `m8_all_privs`@`%` */;
GRANT USAGE ON *.* TO `m8_auth_caching_sha2`@`%`;
GRANT USAGE ON *.* TO `m8_auth_no_pwd`@`%`;
GRANT SELECT ON `m8_db`.* TO `m8_auth_no_pwd`@`%`;
GRANT USAGE ON *.* TO `m8_auth_sha256`@`%`;
GRANT USAGE ON *.* TO `m8_col_priv`@`%`;
GRANT SELECT (`col_a`, `col_underscore_b`), UPDATE (`col_a`) ON `m8_db`.`tab` TO `m8_col_priv`@`%`;
GRANT USAGE ON *.* TO `m8_combined`@`%`;
GRANT SELECT ON `m8_db`.* TO `m8_combined`@`%`;
/*M!100005 SET DEFAULT ROLE 'm8_default_a' FOR 'm8_combined'@'%' */;
GRANT USAGE ON *.* TO `m8_db_underscore`@`%`;
GRANT SELECT ON `m8_under_score_db`.* TO `m8_db_underscore`@`%`;
GRANT USAGE ON *.* TO `m8_default_role_many`@`%`;
/*M!100005 SET DEFAULT ROLE 'm8_default_a' FOR 'm8_default_role_many'@'%' */;
# WARNING: 'm8_default_role_many'@'%' has multiple default roles in the source; MariaDB supports a single default role, dropping 'm8_default_b'
GRANT USAGE ON *.* TO `m8_default_role_one`@`%`;
/*M!100005 SET DEFAULT ROLE 'm8_default_a' FOR 'm8_default_role_one'@'%' */;
GRANT USAGE ON *.* TO `m8_dual_member`@`%`;
GRANT SELECT, RELOAD, PROCESS, SHOW DATABASES, LOCK TABLES, REPLICATION CLIENT, SHOW VIEW, CREATE USER, EVENT, TRIGGER ON *.* TO `m8_dumper`@`%`;
GRANT SELECT ON `mysql`.* TO `m8_dumper`@`%`;
GRANT USAGE ON *.* TO `m8_dyn_all_dropped`@`%`;
# WARNING: dropped MySQL privilege AUDIT_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege BACKUP_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege CLONE_ADMIN (no MariaDB equivalent)
GRANT USAGE ON *.* TO `m8_dyn_flush_collapse`@`%`;
# WARNING: dropped MySQL privilege FLUSH_OPTIMIZER_COSTS (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FLUSH_STATUS (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FLUSH_TABLES (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FLUSH_USER_RESOURCES (no MariaDB equivalent)
GRANT USAGE ON *.* TO `m8_dyn_grant_opt`@`%`;
# WARNING: WITH GRANT OPTION dropped from a translated privilege: MariaDB would make all global privileges of the account grantable
/*M!100005 GRANT BINLOG REPLAY, BINLOG ADMIN, CONNECTION ADMIN ON *.* TO `m8_dyn_grant_opt`@`%` */;
GRANT USAGE ON *.* TO `m8_dyn_mapped`@`%`;
/*M!100005 GRANT BINLOG REPLAY, BINLOG ADMIN ON *.* TO `m8_dyn_mapped`@`%` */;
GRANT USAGE ON *.* TO `m8_dyn_mixed`@`%`;
# WARNING: dropped MySQL privilege AUDIT_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege BACKUP_ADMIN (no MariaDB equivalent)
/*M!100005 GRANT BINLOG REPLAY, BINLOG ADMIN, CONNECTION ADMIN, SHOW CREATE ROUTINE ON *.* TO `m8_dyn_mixed`@`%` */;
GRANT USAGE ON *.* TO `m8_dyn_unmapped`@`%`;
# WARNING: dropped MySQL privilege BACKUP_ADMIN (no MariaDB equivalent)
GRANT USAGE ON *.* TO `m8_edge@in_name`@`%`;
GRANT SELECT ON `m8_db`.* TO `m8_edge@in_name`@`%`;
GRANT USAGE ON *.* TO `m8_edge_wildcard_host`@`192.168.%.%`;
GRANT SELECT ON `m8_db`.* TO `m8_edge_wildcard_host`@`192.168.%.%`;
GRANT USAGE ON *.* TO `m8_failed_login`@`%`;
GRANT USAGE ON *.* TO `m8_func_exec`@`%`;
GRANT EXECUTE ON FUNCTION `m8_db`.`func_one` TO `m8_func_exec`@`%`;
GRANT USAGE ON *.* TO `m8_host_dual`@`%`;
GRANT SELECT ON `m8_db`.`tab2` TO `m8_host_dual`@`%`;
GRANT USAGE ON *.* TO `m8_host_dual`@`localhost`;
GRANT SELECT ON `m8_db`.* TO `m8_host_dual`@`localhost`;
GRANT USAGE ON *.* TO `m8_host_localhost`@`localhost`;
GRANT SELECT ON `m8_db`.* TO `m8_host_localhost`@`localhost`;
GRANT USAGE ON *.* TO `m8_hybrid`@`%`;
GRANT SELECT ON `m8_db`.`tab` TO `m8_hybrid`@`%`;
GRANT USAGE ON *.* TO `m8_hybrid_member`@`%`;
GRANT USAGE ON *.* TO `m8_lock`@`%`;
GRANT USAGE ON *.* TO `m8_max_conn`@`%`;
GRANT PROCESS ON *.* TO `m8_multiscope`@`%`;
GRANT SELECT ON `m8_db`.* TO `m8_multiscope`@`%`;
GRANT INSERT, UPDATE ON `m8_db`.`tab` TO `m8_multiscope`@`%`;
GRANT SELECT, INSERT ON *.* TO `m8_partial_revoke`@`%`;
# WARNING: partial REVOKE has no MariaDB equivalent, ignored: REVOKE INSERT ON `m8_db`.* FROM `m8_partial_revoke`@`%`
#   the wider GRANT remains in effect; re-grant at the narrower scope if needed.
GRANT USAGE ON *.* TO `m8_proc_exec`@`%`;
GRANT EXECUTE ON PROCEDURE `m8_db`.`proc_one` TO `m8_proc_exec`@`%`;
GRANT USAGE ON *.* TO `m8_proxy_target`@`%`;
GRANT USAGE ON *.* TO `m8_proxy_user`@`%`;
GRANT PROXY ON `m8_proxy_target`@`%` TO `m8_proxy_user`@`%`;
GRANT USAGE ON *.* TO `m8_pwd_expire_default`@`%`;
GRANT USAGE ON *.* TO `m8_pwd_expire_interval`@`%`;
GRANT USAGE ON *.* TO `m8_pwd_expire_never`@`%`;
GRANT USAGE ON *.* TO `m8_pwd_expire_now`@`%`;
GRANT USAGE ON *.* TO `m8_pwd_history`@`%`;
GRANT USAGE ON *.* TO `m8_pwd_require_current`@`%`;
GRANT USAGE ON *.* TO `m8_pwd_reuse`@`%`;
GRANT USAGE ON *.* TO `m8_require_cipher`@`%`;
GRANT USAGE ON *.* TO `m8_require_ssl`@`%`;
GRANT USAGE ON *.* TO `m8_require_x509`@`%`;
GRANT USAGE ON *.* TO `m8_resource_full`@`%`;
# WARNING: dropped MySQL privilege CREATE ROLE (no MariaDB equivalent)
# WARNING: dropped MySQL privilege DROP ROLE (no MariaDB equivalent)
GRANT USAGE ON *.* TO `m8_routine_admin`@`%`;
GRANT EXECUTE, CREATE ROUTINE, ALTER ROUTINE ON `m8_db`.* TO `m8_routine_admin`@`%`;
GRANT USAGE ON *.* TO `m8_shared_user_1`@`%`;
GRANT USAGE ON *.* TO `m8_shared_user_2`@`%`;
GRANT USAGE ON *.* TO `m8_shared_user_3`@`%`;
GRANT USAGE ON *.* TO `m8_shared_user_4`@`%`;
GRANT USAGE ON *.* TO `m8_shared_user_5`@`%`;
GRANT USAGE ON *.* TO `m8_simple`@`%`;
GRANT SELECT ON `m8_db`.* TO `m8_simple`@`%`;
GRANT USAGE ON *.* TO `m8_starslash_comment`@`%`;
GRANT USAGE ON *.* TO `m8_starslash_escaped`@`%`;
GRANT SELECT ON *.* TO `m8_static_dyn_mix`@`%`;
# WARNING: dropped MySQL privilege BACKUP_ADMIN (no MariaDB equivalent)
/*M!100005 GRANT BINLOG REPLAY, BINLOG ADMIN ON *.* TO `m8_static_dyn_mix`@`%` */;
GRANT RELOAD, PROCESS ON *.* TO `m8_static_priv`@`%`;
GRANT SELECT, INSERT, UPDATE, DELETE ON `m8_db`.* TO `m8_static_priv`@`%`;
# WARNING: dropped MySQL privilege CREATE ROLE (no MariaDB equivalent)
# WARNING: dropped MySQL privilege DROP ROLE (no MariaDB equivalent)
/*M!100005 GRANT SELECT, INSERT ON *.* TO `m8_static_role_mix`@`%` */;
GRANT USAGE ON *.* TO `m8_system_grants`@`%`;
GRANT SELECT ON `performance_schema`.* TO `m8_system_grants`@`%`;
GRANT SELECT ON `mysql`.`user` TO `m8_system_grants`@`%`;
GRANT USAGE ON *.* TO `m8_unlock`@`%`;
GRANT USAGE ON *.* TO `m8_usage_only`@`%`;
GRANT USAGE ON *.* TO `m8_with_attribute`@`%`;
GRANT USAGE ON *.* TO `m8_with_comment`@`%`;
GRANT USAGE ON *.* TO `m8_用户`@`%`;
GRANT SELECT ON `m8_db`.* TO `m8_用户`@`%`;
GRANT SELECT ON *.* TO `mysql.infoschema`@`localhost`;
# WARNING: dropped MySQL privilege AUDIT_ABORT_EXEMPT (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FIREWALL_EXEMPT (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SYSTEM_USER (no MariaDB equivalent)
GRANT SHUTDOWN, SUPER ON *.* TO `mysql.session`@`localhost`;
# WARNING: dropped MySQL privilege AUDIT_ABORT_EXEMPT (no MariaDB equivalent)
# WARNING: dropped MySQL privilege AUTHENTICATION_POLICY_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege BACKUP_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege CLONE_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FIREWALL_EXEMPT (no MariaDB equivalent)
# WARNING: dropped MySQL privilege PERSIST_RO_VARIABLES_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SESSION_VARIABLES_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SYSTEM_USER (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SYSTEM_VARIABLES_ADMIN (no MariaDB equivalent)
/*M!100005 GRANT CONNECTION ADMIN ON *.* TO `mysql.session`@`localhost` */;
GRANT SELECT ON `performance_schema`.* TO `mysql.session`@`localhost`;
GRANT SELECT ON `mysql`.`user` TO `mysql.session`@`localhost`;
GRANT USAGE ON *.* TO `mysql.sys`@`localhost`;
# WARNING: dropped MySQL privilege AUDIT_ABORT_EXEMPT (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FIREWALL_EXEMPT (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SYSTEM_USER (no MariaDB equivalent)
GRANT TRIGGER ON `sys`.* TO `mysql.sys`@`localhost`;
GRANT SELECT ON `sys`.`sys_config` TO `mysql.sys`@`localhost`;
# WARNING: dropped MySQL privilege CREATE ROLE (no MariaDB equivalent)
# WARNING: dropped MySQL privilege DROP ROLE (no MariaDB equivalent)
/*M!100005 GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, RELOAD, SHUTDOWN, PROCESS, FILE, REFERENCES, INDEX, ALTER, SHOW DATABASES, SUPER, CREATE TEMPORARY TABLES, LOCK TABLES, EXECUTE, REPLICATION SLAVE, REPLICATION CLIENT, CREATE VIEW, SHOW VIEW, CREATE ROUTINE, ALTER ROUTINE, CREATE USER, EVENT, TRIGGER, CREATE TABLESPACE ON *.* TO `root`@`localhost` WITH GRANT OPTION */;
# WARNING: dropped MySQL privilege ALLOW_NONEXISTENT_DEFINER (no MariaDB equivalent)
# WARNING: dropped MySQL privilege APPLICATION_PASSWORD_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege AUDIT_ABORT_EXEMPT (no MariaDB equivalent)
# WARNING: dropped MySQL privilege AUDIT_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege AUTHENTICATION_POLICY_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege BACKUP_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege BINLOG_ENCRYPTION_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege CLONE_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege CREATE_SPATIAL_REFERENCE_SYSTEM (no MariaDB equivalent)
# WARNING: dropped MySQL privilege ENCRYPTION_KEY_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FIREWALL_EXEMPT (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FLUSH_OPTIMIZER_COSTS (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FLUSH_PRIVILEGES (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FLUSH_STATUS (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FLUSH_TABLES (no MariaDB equivalent)
# WARNING: dropped MySQL privilege FLUSH_USER_RESOURCES (no MariaDB equivalent)
# WARNING: dropped MySQL privilege GROUP_REPLICATION_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege GROUP_REPLICATION_STREAM (no MariaDB equivalent)
# WARNING: dropped MySQL privilege INNODB_REDO_LOG_ARCHIVE (no MariaDB equivalent)
# WARNING: dropped MySQL privilege INNODB_REDO_LOG_ENABLE (no MariaDB equivalent)
# WARNING: dropped MySQL privilege MANAGE_DATA_MASKING_POLICY (no MariaDB equivalent)
# WARNING: dropped MySQL privilege OPTIMIZE_LOCAL_TABLE (no MariaDB equivalent)
# WARNING: dropped MySQL privilege PASSWORDLESS_USER_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege PERSIST_RO_VARIABLES_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege RESOURCE_GROUP_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege RESOURCE_GROUP_USER (no MariaDB equivalent)
# WARNING: dropped MySQL privilege ROLE_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SENSITIVE_VARIABLES_OBSERVER (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SERVICE_CONNECTION_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SESSION_VARIABLES_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SYSTEM_USER (no MariaDB equivalent)
# WARNING: dropped MySQL privilege SYSTEM_VARIABLES_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege TABLE_ENCRYPTION_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege TELEMETRY_LOG_ADMIN (no MariaDB equivalent)
# WARNING: dropped MySQL privilege TRANSACTION_GTID_TAG (no MariaDB equivalent)
# WARNING: dropped MySQL privilege XA_RECOVER_ADMIN (no MariaDB equivalent)
# WARNING: WITH GRANT OPTION dropped from a translated privilege: MariaDB would make all global privileges of the account grantable
/*M!100005 GRANT BINLOG REPLAY, BINLOG ADMIN, CONNECTION ADMIN, REPLICATION SLAVE ADMIN, SET USER, SHOW CREATE ROUTINE ON *.* TO `root`@`localhost` */;
GRANT PROXY ON ``@`` TO `root`@`localhost` WITH GRANT OPTION;
/*M!100005 GRANT USAGE ON *.* TO 'm8_admin_chain_root' */;
/*M!100005 GRANT USAGE ON *.* TO 'm8_default_a' */;
/*M!100005 GRANT USAGE ON *.* TO 'm8_default_b' */;
/*M!100005 GRANT USAGE ON *.* TO 'm8_hybrid' */;
/*M!100005 GRANT SELECT ON `m8_db`.`tab` TO 'm8_hybrid' */;
/*M!100203 EXECUTE IMMEDIATE CONCAT('GRANT USAGE ON *.* TO ''m8_role*', '/starslash''') */;
# WARNING: dropped MySQL privilege BACKUP_ADMIN (no MariaDB equivalent)
/*M!100005 GRANT USAGE ON *.* TO 'm8_role_a' */;
/*M!100005 GRANT SELECT ON `m8_db`.* TO 'm8_role_a' */;
/*M!100005 GRANT USAGE ON *.* TO 'm8_role_b' */;
/*M!100005 GRANT INSERT ON `m8_db`.* TO 'm8_role_b' */;
/*M!100005 GRANT USAGE ON *.* TO 'm8_role_c' */;
/*M!100005 GRANT UPDATE ON `m8_db`.* TO 'm8_role_c' */;
/*M!100005 GRANT SHOW DATABASES ON *.* TO 'm8_role_d' */;
/*M!100005 GRANT USAGE ON *.* TO 'm8_role_dual' */;
/*M!100005 GRANT SELECT ON `m8_db`.`tab` TO 'm8_role_dual' */;
/*M!100005 GRANT USAGE ON *.* TO 'm8_role_dual' */;
/*M!100005 GRANT INSERT ON `m8_db`.`tab` TO 'm8_role_dual' */;
/*M!100005 GRANT USAGE ON *.* TO 'm8_shared_role' */;
/*M!100005 GRANT SELECT ON `m8_db`.`tab` TO 'm8_shared_role' */;
/*M!100005 GRANT EXECUTE ON PROCEDURE `m8_db`.`proc_one` TO 'm8_shared_role' */;
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

-- Dump completed on 2026-10-20 17:29:06
