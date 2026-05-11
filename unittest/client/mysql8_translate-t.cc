/*
   Copyright (c) 2026, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include <my_global.h>
#include <my_sys.h>
#include "mysql8_translate.h"

#include <tap.h>
#include <string>

/*
  Inputs marked "captured" are SHOW GRANTS / SHOW CREATE USER output of
  a MySQL 26.7.0 or 5.7 server, the rest are hand-written edge cases.
*/

/* Run fn(out) and return what it wrote to out. */
template <class F> static std::string capture(F fn)
{
  FILE *f= tmpfile();
  if (!f)
  {
    diag("tmpfile() failed");
    return "<tmpfile() failed>";
  }
  fn(f);
  std::string res;
  char buf[512];
  size_t n;
  rewind(f);
  while ((n= fread(buf, 1, sizeof(buf), f)) > 0)
    res.append(buf, n);
  fclose(f);
  return res;
}

static void check_grant(const char *name, const char *grant, bool handled,
                        const char *expected, const char *role_name= NULL)
{
  bool res= false;
  std::string out= capture([&](FILE *f)
                           { res= translate_mysql8_grant(grant, f, role_name); });
  ok(res == handled && out == expected, "%s", name);
  if (res != handled || out != expected)
    diag("handled=%d, output:\n%s", (int) res, out.c_str());
}

static void check_user(const char *name, const char *user_str,
                       bool replace_into, bool ignore, const char *expected)
{
  std::string out= capture([&](FILE *f)
    { translate_mysql8_create_user(user_str, f, replace_into, ignore); });
  ok(out == expected, "%s", name);
  if (out != expected)
    diag("output:\n%s", out.c_str());
}

#define DROPPED(priv) \
  "# WARNING: dropped MySQL privilege " priv " (no MariaDB equivalent)\n"
#define GRANT_OPT_DROPPED \
  "# WARNING: WITH GRANT OPTION dropped from a translated privilege: " \
  "MariaDB would make all global privileges of the account grantable\n"
#define POLICY_DROPPED(clause) \
  "# WARNING: password policy clause dropped (no MariaDB equivalent): " \
  clause "\n"
#define OPTION_DROPPED(clause) \
  "# WARNING: account option dropped (no MariaDB equivalent): " clause "\n"
#define AS_DROPPED(clause) \
  "# WARNING: partial-revokes AS clause dropped (no MariaDB equivalent): " \
  clause "; the grant below is wider than on the MySQL source\n"

static void test_grant()
{
  check_grant("captured: dynamic privileges mapped or dropped",
    "GRANT AUDIT_ABORT_EXEMPT,AUTHENTICATION_POLICY_ADMIN,BACKUP_ADMIN,"
    "CLONE_ADMIN,CONNECTION_ADMIN,FIREWALL_EXEMPT,PERSIST_RO_VARIABLES_ADMIN,"
    "SESSION_VARIABLES_ADMIN,SYSTEM_USER,SYSTEM_VARIABLES_ADMIN "
    "ON *.* TO `mysql.session`@`localhost`", true,
    DROPPED("AUDIT_ABORT_EXEMPT")
    DROPPED("AUTHENTICATION_POLICY_ADMIN")
    DROPPED("BACKUP_ADMIN")
    DROPPED("CLONE_ADMIN")
    DROPPED("FIREWALL_EXEMPT")
    DROPPED("PERSIST_RO_VARIABLES_ADMIN")
    DROPPED("SESSION_VARIABLES_ADMIN")
    DROPPED("SYSTEM_USER")
    DROPPED("SYSTEM_VARIABLES_ADMIN")
    "/*M!100005 GRANT CONNECTION ADMIN ON *.* TO `mysql.session`@`localhost` */;\n");

  check_grant("captured: static privileges pass through",
    "GRANT SHUTDOWN, SUPER ON *.* TO `mysql.session`@`localhost`", false, "");

  check_grant("unlisted dynamic privilege is dropped",
    "GRANT MANAGE_DATA_MASKING_POLICY,SELECT ON *.* TO `u`@`h`", true,
    DROPPED("MANAGE_DATA_MASKING_POLICY")
    "/*M!100005 GRANT SELECT ON *.* TO `u`@`h` */;\n");

  check_grant("two privileges mapped to one are deduplicated",
    "GRANT BINLOG_ADMIN,REPLICATION_APPLIER ON *.* TO `u`@`h`", true,
    "/*M!100005 GRANT BINLOG REPLAY, BINLOG ADMIN ON *.* TO `u`@`h` */;\n");

  check_grant("privileges whose MariaDB counterpart is wider are dropped",
    "GRANT ALLOW_NONEXISTENT_DEFINER,SERVICE_CONNECTION_ADMIN,SET_ANY_DEFINER "
    "ON *.* TO `u`@`h`", true,
    DROPPED("ALLOW_NONEXISTENT_DEFINER")
    DROPPED("SERVICE_CONNECTION_ADMIN")
    "/*M!100005 GRANT SET USER ON *.* TO `u`@`h` */;\n");

  check_grant("grant option of a translated privilege is dropped",
    "GRANT BINLOG_ADMIN ON *.* TO `u`@`h` WITH GRANT OPTION", true,
    GRANT_OPT_DROPPED
    "/*M!100005 GRANT BINLOG REPLAY, BINLOG ADMIN ON *.* TO `u`@`h` */;\n");

  check_grant("grant option of a translated privilege on a role grantee",
    "GRANT BINLOG_ADMIN ON *.* TO `r1`@`%` WITH GRANT OPTION", true,
    GRANT_OPT_DROPPED
    "/*M!100005 GRANT BINLOG REPLAY, BINLOG ADMIN ON *.* TO `r1` */;\n",
    "`r1`");

  check_grant("REPLICATION_SLAVE_ADMIN, SHOW_ROUTINE and SET_ANY_DEFINER map",
    "GRANT REPLICATION_SLAVE_ADMIN,SHOW_ROUTINE,SET_ANY_DEFINER,"
    "CONNECTION_ADMIN,REPLICATION_APPLIER ON *.* TO `u`@`h`", true,
    "/*M!100005 GRANT REPLICATION SLAVE ADMIN, SHOW CREATE ROUTINE, SET USER, "
    "CONNECTION ADMIN, BINLOG REPLAY ON *.* TO `u`@`h` */;\n");

  check_grant("grant option of a static privilege survives a dropped one",
    "GRANT SELECT, CREATE ROLE ON *.* TO `u`@`h` WITH GRANT OPTION", true,
    DROPPED("CREATE ROLE")
    "/*M!100005 GRANT SELECT ON *.* TO `u`@`h` WITH GRANT OPTION */;\n");

  check_grant("column list is one privilege",
    "GRANT SELECT (a, b), BACKUP_ADMIN ON `d`.`t` TO `u`@`h`", true,
    DROPPED("BACKUP_ADMIN")
    "/*M!100005 GRANT SELECT (a, b) ON `d`.`t` TO `u`@`h` */;\n");

  check_grant("role grant is skipped",
    "GRANT `r1`@`%` TO `u`@`%`", true, "");

  check_grant("AS clause is dropped",
    "GRANT SELECT ON *.* TO `u`@`h` WITH GRANT OPTION AS `root`@`localhost`",
    true,
    AS_DROPPED("AS `root`@`localhost`")
    "/*M!100005 GRANT SELECT ON *.* TO `u`@`h` WITH GRANT OPTION */;\n");

  check_grant("AS clause is not warned about when no privilege survives",
    "GRANT BACKUP_ADMIN ON *.* TO `u`@`h` AS `root`@`localhost`", true,
    DROPPED("BACKUP_ADMIN"));

  check_grant("backtick name ending in a backslash",
    "GRANT SELECT ON `a\\`.* TO `u`@`h` AS `root`@`localhost`", true,
    AS_DROPPED("AS `root`@`localhost`")
    "/*M!100005 GRANT SELECT ON `a\\`.* TO `u`@`h` */;\n");

  check_grant("role grantee loses its host",
    "GRANT SELECT ON `d`.* TO `r1`@`%`", true,
    "/*M!100005 GRANT SELECT ON `d`.* TO `r1` */;\n", "`r1`");

  check_grant("role grantee with an AS clause",
    "GRANT SELECT ON `d`.* TO `r1`@`%` AS `root`@`localhost`", true,
    AS_DROPPED("AS `root`@`localhost`")
    "/*M!100005 GRANT SELECT ON `d`.* TO `r1` */;\n", "`r1`");

  check_grant("GRANT longer than the stack buffer",
    ("GRANT SELECT ON `" + std::string(1100, 'd') + "`.* TO `r1`@`%`").c_str(),
    true,
    ("/*M!100005 GRANT SELECT ON `" + std::string(1100, 'd') +
     "`.* TO `r1` */;\n").c_str(), "`r1`");

  check_grant("role grantee with GRANT OPTION and USAGE",
    "GRANT USAGE ON *.* TO `r1`@`%` WITH GRANT OPTION", true,
    "/*M!100005 GRANT USAGE ON *.* TO `r1` WITH GRANT OPTION */;\n", "`r1`");

  check_grant("role name containing a comment terminator is split",
    "GRANT USAGE ON *.* TO `r*/x`@`%`", true,
    "/*M!100203 EXECUTE IMMEDIATE CONCAT('GRANT USAGE ON *.* TO `r*', "
    "'/x`') */;\n", "`r*/x`");

  check_grant("comment terminator in an identifier is split",
    "GRANT SELECT, BACKUP_ADMIN ON `d*/x`.* TO `u`@`h`", true,
    DROPPED("BACKUP_ADMIN")
    "/*M!100203 EXECUTE IMMEDIATE CONCAT('GRANT SELECT ON `d*', "
    "'/x`.* TO `u`@`h`') */;\n");
}

static void test_revoke()
{
  std::string out= capture([](FILE *f)
    { translate_mysql8_revoke("REVOKE INSERT ON `d\nx`.* FROM `u`@`h`", f); });
  ok(out ==
     "# WARNING: partial REVOKE has no MariaDB equivalent, ignored: "
     "REVOKE INSERT ON `d x`.* FROM `u`@`h`\n"
     "#   the wider GRANT remains in effect; re-grant at the narrower "
     "scope if needed.\n", "partial REVOKE is a warning, newline folded");
}

static void test_create_user()
{
  check_user("captured: clauses without MariaDB equivalent are dropped",
    " `root`@`localhost` IDENTIFIED WITH 'caching_sha2_password' REQUIRE NONE "
    "PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK PASSWORD HISTORY DEFAULT "
    "PASSWORD REUSE INTERVAL DEFAULT PASSWORD REQUIRE CURRENT DEFAULT",
    false, false,
    "CREATE USER `root`@`localhost` IDENTIFIED WITH 'caching_sha2_password' "
    "REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;\n");

  check_user("captured: locked account keeps ACCOUNT LOCK",
    " `mysql.session`@`localhost` IDENTIFIED WITH 'caching_sha2_password' "
    "AS '$A$005$THISISACOMBINATIONOFINVALIDSALTANDPASSWORDTHATMUSTNEVERBRBEUSED' "
    "REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT LOCK PASSWORD HISTORY DEFAULT "
    "PASSWORD REUSE INTERVAL DEFAULT PASSWORD REQUIRE CURRENT DEFAULT",
    false, false,
    "CREATE USER `mysql.session`@`localhost` IDENTIFIED WITH "
    "'caching_sha2_password' AS "
    "'$A$005$THISISACOMBINATIONOFINVALIDSALTANDPASSWORDTHATMUSTNEVERBRBEUSED' "
    "REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;\n");

  check_user("unsupported plugin: IDENTIFIED dropped, account locked",
    " `u`@`h` IDENTIFIED WITH 'sha256_password' AS 'x' REQUIRE NONE "
    "PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK", false, false,
    "CREATE USER `u`@`h` REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;\n"
    "# WARNING: auth plugin 'sha256_password' not shipped with MariaDB; "
    "user created with no password and ACCOUNT LOCK - set a password and "
    "ACCOUNT UNLOCK before login\n");

  check_user("unsupported plugin without an ACCOUNT clause",
    " `u`@`h` IDENTIFIED WITH 'sha256_password' AS 'x'", false, false,
    "CREATE USER `u`@`h` ACCOUNT LOCK;\n"
    "# WARNING: auth plugin 'sha256_password' not shipped with MariaDB; "
    "user created with no password and ACCOUNT LOCK - set a password and "
    "ACCOUNT UNLOCK before login\n");

  check_user("second authentication factor is dropped",
    " `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' AND IDENTIFIED "
    "WITH 'authentication_ldap_simple' AS 'x' REQUIRE NONE", false, false,
    "CREATE USER `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' "
    "REQUIRE NONE;\n"
    "# WARNING: additional authentication factor 'authentication_ldap_simple' "
    "dropped: MariaDB cannot require it\n");

  check_user("second factor with a supported plugin is warned about",
    " `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' AND IDENTIFIED "
    "WITH 'mysql_native_password' AS 'h2' REQUIRE NONE", false, false,
    "CREATE USER `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' "
    "REQUIRE NONE;\n"
    "# WARNING: additional authentication factor 'mysql_native_password' "
    "dropped: MariaDB cannot require it\n");

  check_user("newline in a plugin name is folded",
    " `u`@`h` IDENTIFIED WITH 'bad\nDROP USER x;\nplug' AS 'x'", false, false,
    "CREATE USER `u`@`h` ACCOUNT LOCK;\n"
    "# WARNING: auth plugin 'bad DROP USER x; plug' not shipped with "
    "MariaDB; user created with no password and ACCOUNT LOCK - set a "
    "password and ACCOUNT UNLOCK before login\n");

  {
    std::string name(700, 'p');
    check_user("long plugin name keeps the warning on its own line",
      (" `u`@`h` IDENTIFIED WITH '" + name + "' AS 'x'").c_str(), false, false,
      ("CREATE USER `u`@`h` ACCOUNT LOCK;\n"
       "# WARNING: auth plugin '" + name + "' not shipped with MariaDB; user "
       "created with no password and ACCOUNT LOCK - set a password and "
       "ACCOUNT UNLOCK before login\n").c_str());
  }

  check_user("--replace",
    " `u`@`h` IDENTIFIED WITH 'mysql_native_password' AS '*ABC' REQUIRE NONE",
    true, false,
    "CREATE /*M!100103 OR REPLACE */ USER `u`@`h` IDENTIFIED "
    "WITH 'mysql_native_password' AS '*ABC' REQUIRE NONE;\n");

  check_user("--ignore",
    " `u`@`h` IDENTIFIED WITH 'mysql_native_password' AS '*ABC' REQUIRE NONE",
    false, true,
    "CREATE USER IF NOT EXISTS `u`@`h` IDENTIFIED "
    "WITH 'mysql_native_password' AS '*ABC' REQUIRE NONE;\n");

  check_user("dropped clauses around kept ones",
    " `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' COMMENT 'c' "
    "FAILED_LOGIN_ATTEMPTS 3 REQUIRE SSL PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK "
    "PASSWORD REQUIRE CURRENT DEFAULT PASSWORD_LOCK_TIME 2 ATTRIBUTE '{}'",
    false, false,
    "CREATE USER `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' "
    "REQUIRE SSL PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK;\n"
    OPTION_DROPPED("COMMENT 'c'")
    OPTION_DROPPED("FAILED_LOGIN_ATTEMPTS 3")
    OPTION_DROPPED("PASSWORD_LOCK_TIME 2")
    OPTION_DROPPED("ATTRIBUTE '{}'"));

  check_user("captured from 5.7: sha256_password is dropped and locked",
    " `u`@`h` IDENTIFIED WITH 'sha256_password' AS '$5$x' REQUIRE NONE "
    "PASSWORD EXPIRE DEFAULT ACCOUNT UNLOCK", false, false,
    "CREATE USER `u`@`h` REQUIRE NONE PASSWORD EXPIRE DEFAULT ACCOUNT LOCK;\n"
    "# WARNING: auth plugin 'sha256_password' not shipped with MariaDB; "
    "user created with no password and ACCOUNT LOCK - set a password and "
    "ACCOUNT UNLOCK before login\n");

  check_user("captured from 5.7: native password and policy are kept",
    " `u`@`h` IDENTIFIED WITH 'mysql_native_password' AS '*ABC' REQUIRE "
    "SUBJECT '/C=SE' ISSUER '/C=SE' CIPHER 'AES256-SHA' WITH "
    "MAX_QUERIES_PER_HOUR 10 PASSWORD EXPIRE INTERVAL 30 DAY ACCOUNT UNLOCK",
    false, false,
    "CREATE USER `u`@`h` IDENTIFIED WITH 'mysql_native_password' AS '*ABC' "
    "REQUIRE SUBJECT '/C=SE' ISSUER '/C=SE' CIPHER 'AES256-SHA' WITH "
    "MAX_QUERIES_PER_HOUR 10 PASSWORD EXPIRE INTERVAL 30 DAY ACCOUNT UNLOCK;\n");

  check_user("keyword inside a quoted name is not a clause",
    " `x REQUIRE y`@`h` REQUIRE SSL", false, false,
    "CREATE USER `x REQUIRE y`@`h` REQUIRE SSL;\n");

  check_user("escaped quote inside the hash",
    " `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS '$A$005$x\\'y REQUIRE "
    "z' PASSWORD HISTORY DEFAULT", false, false,
    "CREATE USER `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS "
    "'$A$005$x\\'y REQUIRE z';\n");

  check_user("non-default password policy clauses are warned about",
    " `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' "
    "PASSWORD HISTORY 5 PASSWORD REUSE INTERVAL 30 DAY "
    "PASSWORD REQUIRE CURRENT OPTIONAL", false, false,
    "CREATE USER `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1';\n"
    POLICY_DROPPED("PASSWORD HISTORY 5")
    POLICY_DROPPED("PASSWORD REUSE INTERVAL 30 DAY")
    POLICY_DROPPED("PASSWORD REQUIRE CURRENT OPTIONAL"));

  check_user("account options are warned about",
    " `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' "
    "FAILED_LOGIN_ATTEMPTS 3 PASSWORD_LOCK_TIME UNBOUNDED COMMENT 'a\nb' "
    "ATTRIBUTE '{\"x\": 1}'", false, false,
    "CREATE USER `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1';\n"
    OPTION_DROPPED("FAILED_LOGIN_ATTEMPTS 3")
    OPTION_DROPPED("PASSWORD_LOCK_TIME UNBOUNDED")
    OPTION_DROPPED("COMMENT 'a b'")
    OPTION_DROPPED("ATTRIBUTE '{\"x\": 1}'"));

  check_user("zero values are the absence of a policy",
    " `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' "
    "PASSWORD HISTORY 0 PASSWORD REUSE INTERVAL 0 DAY "
    "FAILED_LOGIN_ATTEMPTS 0 PASSWORD_LOCK_TIME 0", false, false,
    "CREATE USER `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1';\n");

  check_user("a value starting with 0 is a policy",
    " `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' "
    "PASSWORD HISTORY 05", false, false,
    "CREATE USER `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1';\n"
    POLICY_DROPPED("PASSWORD HISTORY 05"));

  check_user("bare PASSWORD REQUIRE CURRENT is warned about",
    " `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' "
    "PASSWORD REQUIRE CURRENT", false, false,
    "CREATE USER `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1';\n"
    POLICY_DROPPED("PASSWORD REQUIRE CURRENT"));

  check_user("unsupported first plugin and a second factor",
    " `u`@`h` IDENTIFIED WITH 'sha256_password' AS 'x' AND IDENTIFIED WITH "
    "'caching_sha2_password' AS 'y' REQUIRE NONE", false, false,
    "CREATE USER `u`@`h` REQUIRE NONE ACCOUNT LOCK;\n"
    "# WARNING: auth plugin 'sha256_password' not shipped with MariaDB; "
    "user created with no password and ACCOUNT LOCK - set a password and "
    "ACCOUNT UNLOCK before login\n"
    "# WARNING: additional authentication factor 'caching_sha2_password' "
    "dropped: MariaDB cannot require it\n");

  check_user("DEFAULT ROLE is dropped",
    " `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' DEFAULT ROLE "
    "`r`@`%` REQUIRE NONE", false, false,
    "CREATE USER `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' "
    "REQUIRE NONE;\n");
}

static void test_emit()
{
  std::string out= capture([](FILE *f)
    { dump_emit_comment_warning(f, "a %s b", "x\ny\rz"); });
  ok(out == "# a x y z b\n", "CR/LF are folded in a comment warning");

  out= capture([](FILE *f)
    { dump_emit_versioned_wrap(f, "/*M!100005 ", "DROP ROLE %s", "`a*/b`"); });
  ok(out == "/*M!100203 EXECUTE IMMEDIATE CONCAT('DROP ROLE `a*', '/b`') "
            "*/;\n", "comment terminator is split");

  out= capture([](FILE *f)
    { dump_emit_versioned_wrap(f, "/*M!100005 ", "DROP ROLE %s",
                               "'a\\*/*/'"); });
  ok(out == "/*M!100203 EXECUTE IMMEDIATE CONCAT('DROP ROLE ''a\\\\*', '/*', "
            "'/''') */;\n", "quote and backslash escaped, terminators split");

  out= capture([](FILE *f)
    { dump_put_comment_safe_literal(f, "'a*/b'"); });
  ok(out == "CONCAT('a*', '/b')", "quoted literal is split");

  out= capture([](FILE *f)
    { dump_put_comment_safe_literal(f, "'ab'"); });
  ok(out == "'ab'", "literal without a terminator is unchanged");
}

void dump_fatal(const char *msg)
{
  fprintf(stderr, "%s\n", msg);
  exit(4);
}

int main(int, char **argv)
{
  MY_INIT(argv[0]);
  plan(NO_PLAN);
  test_grant();
  test_revoke();
  test_create_user();
  test_emit();
  my_end(0);
  return exit_status();
}
