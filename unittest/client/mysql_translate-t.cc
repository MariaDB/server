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
#include "mysql_translate.h"
#include "mysql_roles.h"

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

static void report(const char *name, bool pass, const std::string &out)
{
  ok(pass, "%s", name);
  if (!pass)
    diag("output:\n%s", out.c_str());
}

static void check_grant(const char *name, const char *grant, bool handled,
                        const char *expected, const char *role_name= NULL,
                        bool drop_global_option= false)
{
  bool res= false;
  std::string out= capture([&](FILE *f)
    { res= translate_mysql_grant(grant, f, role_name, drop_global_option); });
  /* @CTX@ is what a warning says about the statement: from " ON ", the role */
  const char *on= strstr(grant, " ON ");
  std::string ctx= std::string(on ? on : "") +
                   (role_name ? std::string(", role ") + role_name : "");
  std::string exp= expected;
  for (size_t at; (at= exp.find("@CTX@")) != std::string::npos; )
    exp.replace(at, 5, ctx);
  report(name, res == handled && out == exp, out);
}

static void check_user(const char *name, const char *user_str,
                       bool replace_into, bool ignore, const char *expected)
{
  std::string out= capture([&](FILE *f)
    { translate_mysql_create_user(user_str, f, replace_into, ignore); });
  report(name, out == expected, out);
}

#define DROPPED(priv) \
  "# WARNING: dropped MySQL privilege(s) " priv \
  " (no MariaDB equivalent):@CTX@\n"
#define GRANT_OPT_DROPPED \
  "# WARNING: WITH GRANT OPTION dropped from a translated privilege: " \
  "MariaDB would make all global privileges of the account grantable:@CTX@\n"
#define OPT_DROPPED \
  "# WARNING: WITH GRANT OPTION dropped: MariaDB would let the account " \
  "grant its translated privileges, which MySQL did not:@CTX@\n"
#define DENY_NEEDS_13_1_1(ctx) \
  "# WARNING: a partial REVOKE is a DENY, which needs MariaDB 13.1.1: an " \
  "older server does not run it, and the account is wider than on the " \
  "MySQL source:" ctx "\n"
#define NOGATE \
  "# WARNING: the statement below has no version gate: it has a comment " \
  "end, and an older server rejects it\n"
#define POLICY_DROPPED(clause) \
  "# WARNING: password policy clause dropped (no MariaDB equivalent): " \
  clause "\n"
#define OPTION_DROPPED(clause) \
  "# WARNING: account option dropped (no MariaDB equivalent): " clause "\n"
#define AUTH_LOCKED(plugin) \
  "# WARNING: auth plugin '" plugin "' not shipped with MariaDB; user " \
  "created with no password and ACCOUNT LOCK - set a password and " \
  "ACCOUNT UNLOCK before login\n"
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
    DROPPED("AUDIT_ABORT_EXEMPT, AUTHENTICATION_POLICY_ADMIN, "
            "BACKUP_ADMIN, CLONE_ADMIN, FIREWALL_EXEMPT, "
            "PERSIST_RO_VARIABLES_ADMIN, SESSION_VARIABLES_ADMIN, "
            "SYSTEM_USER, SYSTEM_VARIABLES_ADMIN")
    "/*M!110400 GRANT CONNECTION ADMIN ON *.* TO `mysql.session`@`localhost` */;\n");

  check_grant("captured: static privileges pass through",
    "GRANT SHUTDOWN, SUPER ON *.* TO `mysql.session`@`localhost`", false, "");

  check_grant("unlisted dynamic privilege is dropped",
    "GRANT MANAGE_DATA_MASKING_POLICY,SELECT ON *.* TO `u`@`h`", true,
    DROPPED("MANAGE_DATA_MASKING_POLICY")
    "/*M!110400 GRANT SELECT ON *.* TO `u`@`h` */;\n");

  check_grant("two privileges mapped to one are deduplicated",
    "GRANT BINLOG_ADMIN,REPLICATION_APPLIER ON *.* TO `u`@`h`", true,
    "/*M!110400 GRANT BINLOG REPLAY, BINLOG ADMIN ON *.* TO `u`@`h` */;\n");

  check_grant("privileges whose MariaDB counterpart is wider are dropped",
    "GRANT ALLOW_NONEXISTENT_DEFINER,SERVICE_CONNECTION_ADMIN,SET_ANY_DEFINER "
    "ON *.* TO `u`@`h`", true,
    DROPPED("ALLOW_NONEXISTENT_DEFINER, SERVICE_CONNECTION_ADMIN")
    "/*M!110400 GRANT SET USER ON *.* TO `u`@`h` */;\n");

  check_grant("SET_USER_ID and SET_ANY_DEFINER give one SET USER",
    "GRANT SET_ANY_DEFINER,SET_USER_ID ON *.* TO `u`@`h`", true,
    "/*M!110400 GRANT SET USER ON *.* TO `u`@`h` */;\n");

  check_grant("a privilege MariaDB does not know is dropped, a column list is kept",
    "GRANT SELECT (a, b), CREATE TABLESPACE, FOO BAR ON `d`.`t` TO `u`@`h`",
    true,
    DROPPED("FOO BAR")
    "/*M!110400 GRANT SELECT (a, b), CREATE TABLESPACE ON `d`.`t` "
    "TO `u`@`h` */;\n");

  check_grant("grant option of a translated privilege is dropped",
    "GRANT BINLOG_ADMIN ON *.* TO `u`@`h` WITH GRANT OPTION", true,
    GRANT_OPT_DROPPED
    "/*M!110400 GRANT BINLOG REPLAY, BINLOG ADMIN ON *.* TO `u`@`h` */;\n");

  check_grant("grant option of a translated privilege on a role grantee",
    "GRANT BINLOG_ADMIN ON *.* TO `r1`@`%` WITH GRANT OPTION", true,
    GRANT_OPT_DROPPED
    "/*M!110400 GRANT BINLOG REPLAY, BINLOG ADMIN ON *.* TO `r1` */;\n",
    "`r1`");

  check_grant("REPLICATION_SLAVE_ADMIN, SHOW_ROUTINE and SET_ANY_DEFINER map",
    "GRANT REPLICATION_SLAVE_ADMIN,SHOW_ROUTINE,SET_ANY_DEFINER,"
    "CONNECTION_ADMIN,REPLICATION_APPLIER ON *.* TO `u`@`h`", true,
    "/*M!110400 GRANT REPLICATION SLAVE ADMIN, SHOW CREATE ROUTINE, "
    "SET USER, CONNECTION ADMIN, BINLOG REPLAY ON *.* TO `u`@`h` */;\n");

  check_grant("SHOW_ROUTINE alone maps",
    "GRANT SHOW_ROUTINE ON *.* TO `u`@`h` WITH GRANT OPTION", true,
    GRANT_OPT_DROPPED
    "/*M!110400 GRANT SHOW CREATE ROUTINE ON *.* TO `u`@`h` */;\n");

  check_grant("grant option of a static privilege survives a dropped one",
    "GRANT SELECT, CREATE ROLE ON *.* TO `u`@`h` WITH GRANT OPTION", true,
    DROPPED("CREATE ROLE")
    "/*M!110400 GRANT SELECT ON *.* TO `u`@`h` WITH GRANT OPTION */;\n");

  check_grant("column list is one privilege",
    "GRANT SELECT (a, b), BACKUP_ADMIN ON `d`.`t` TO `u`@`h`", true,
    DROPPED("BACKUP_ADMIN")
    "/*M!110400 GRANT SELECT (a, b) ON `d`.`t` TO `u`@`h` */;\n");

  check_grant("role grant is skipped",
    "GRANT `r1`@`%` TO `u`@`%`", true, "");

  check_grant("AS clause is dropped",
    "GRANT SELECT ON *.* TO `u`@`h` WITH GRANT OPTION AS `root`@`localhost`",
    true,
    AS_DROPPED("AS `root`@`localhost`")
    "/*M!110400 GRANT SELECT ON *.* TO `u`@`h` WITH GRANT OPTION */;\n");

  check_grant("AS clause is not warned about when no privilege survives",
    "GRANT BACKUP_ADMIN ON *.* TO `u`@`h` AS `root`@`localhost`", true,
    DROPPED("BACKUP_ADMIN"));

  check_grant("backtick name ending in a backslash",
    "GRANT SELECT ON `a\\`.* TO `u`@`h` AS `root`@`localhost`", true,
    AS_DROPPED("AS `root`@`localhost`")
    "/*M!110400 GRANT SELECT ON `a\\`.* TO `u`@`h` */;\n");

  check_grant("role grantee loses its host",
    "GRANT SELECT ON `d`.* TO `r1`@`%`", true,
    "/*M!110400 GRANT SELECT ON `d`.* TO `r1` */;\n", "`r1`");

  check_grant("role grantee with an AS clause",
    "GRANT SELECT ON `d`.* TO `r1`@`%` AS `root`@`localhost`", true,
    AS_DROPPED("AS `root`@`localhost`")
    "/*M!110400 GRANT SELECT ON `d`.* TO `r1` */;\n", "`r1`");

  check_grant("GRANT longer than the stack buffer",
    ("GRANT SELECT ON `" + std::string(1100, 'd') + "`.* TO `r1`@`%`").c_str(),
    true,
    ("/*M!110400 GRANT SELECT ON `" + std::string(1100, 'd') +
     "`.* TO `r1` */;\n").c_str(), "`r1`");

  check_grant("role grantee with GRANT OPTION and USAGE",
    "GRANT USAGE ON *.* TO `r1`@`%` WITH GRANT OPTION", true,
    "/*M!110400 GRANT USAGE ON *.* TO `r1` WITH GRANT OPTION */;\n", "`r1`");

  check_grant("role name containing a comment terminator is plain",
    "GRANT USAGE ON *.* TO `r*/x`@`%`", true,
    NOGATE "GRANT USAGE ON *.* TO `r*/x`;\n", "`r*/x`");

  check_grant("comment terminator in an identifier is plain",
    "GRANT SELECT, BACKUP_ADMIN ON `d*/x`.* TO `u`@`h`", true,
    DROPPED("BACKUP_ADMIN")
    NOGATE "GRANT SELECT ON `d*/x`.* TO `u`@`h`;\n");

  /* drop_global_option: the account also has a mapped privilege without it */
  check_grant("global option dropped: static grant",
    "GRANT SELECT ON *.* TO `u`@`h` WITH GRANT OPTION", true,
    OPT_DROPPED "/*M!110400 GRANT SELECT ON *.* TO `u`@`h` */;\n", NULL, true);

  check_grant("global option dropped: USAGE",
    "GRANT USAGE ON *.* TO `u`@`h` WITH GRANT OPTION", true,
    OPT_DROPPED "/*M!110400 GRANT USAGE ON *.* TO `u`@`h` */;\n", NULL, true);

  check_grant("global option dropped: with a dropped privilege",
    "GRANT SELECT, CREATE ROLE ON *.* TO `u`@`h` WITH GRANT OPTION", true,
    DROPPED("CREATE ROLE")
    OPT_DROPPED "/*M!110400 GRANT SELECT ON *.* TO `u`@`h` */;\n", NULL, true);

  check_grant("global option dropped: role grantee",
    "GRANT SELECT ON *.* TO `r1`@`%` WITH GRANT OPTION", true,
    OPT_DROPPED "/*M!110400 GRANT SELECT ON *.* TO `r1` */;\n", "`r1`", true);

  check_grant("global option dropped: AS clause kept apart",
    "GRANT SELECT ON *.* TO `u`@`h` WITH GRANT OPTION AS `root`@`localhost`",
    true,
    AS_DROPPED("AS `root`@`localhost`")
    OPT_DROPPED "/*M!110400 GRANT SELECT ON *.* TO `u`@`h` */;\n", NULL, true);

  check_grant("global option dropped: a mapped line says why",
    "GRANT BINLOG_ADMIN ON *.* TO `u`@`h` WITH GRANT OPTION", true,
    GRANT_OPT_DROPPED
    "/*M!110400 GRANT BINLOG REPLAY, BINLOG ADMIN ON *.* TO `u`@`h` */;\n",
    NULL, true);

  check_grant("global option dropped: not at database level",
    "GRANT SELECT ON `d`.* TO `u`@`h` WITH GRANT OPTION", false, "", NULL, true);

  check_grant("global option dropped: nothing to drop",
    "GRANT SELECT ON *.* TO `u`@`h`", false, "", NULL, true);
}

static void check_scan(const char *name, const char *grant, bool global_option,
                       bool mapped_without_option)
{
  Mysql_grant_scan scan= {false, false};
  scan_mysql_grant(grant, &scan);
  report(name, scan.global_option == global_option &&
               scan.mapped_without_option == mapped_without_option,
         std::string(grant));
}

/* The rows of one account, in the order SHOW GRANTS gives them */
static void check_scan_rows(const char *name,
                            std::initializer_list<const char*> rows,
                            bool drop_global_option)
{
  Mysql_grant_scan scan= {false, false};
  std::string all;
  for (const char *row : rows)
  {
    scan_mysql_grant(row, &scan);
    all+= std::string(row) + "\n";
  }
  report(name, scan.drop_global_option() == drop_global_option, all);
}

static void test_scan()
{
  check_scan("scan: static with the option",
    "GRANT SELECT ON *.* TO `u`@`h` WITH GRANT OPTION", true, false);
  check_scan("scan: USAGE with the option",
    "GRANT USAGE ON *.* TO `u`@`h` WITH GRANT OPTION", true, false);
  check_scan("scan: static without the option",
    "GRANT SELECT ON *.* TO `u`@`h`", false, false);
  check_scan("scan: mapped without the option",
    "GRANT CONNECTION_ADMIN ON *.* TO `u`@`h`", false, true);
  check_scan("scan: mapped with the option",
    "GRANT CONNECTION_ADMIN ON *.* TO `u`@`h` WITH GRANT OPTION", false, false);
  check_scan("scan: dropped only, with the option",
    "GRANT BACKUP_ADMIN ON *.* TO `u`@`h` WITH GRANT OPTION", false, false);
  check_scan("scan: dropped only, without the option",
    "GRANT BACKUP_ADMIN ON *.* TO `u`@`h`", false, false);
  check_scan("scan: option at database level",
    "GRANT SELECT ON `d`.* TO `u`@`h` WITH GRANT OPTION", false, false);
  check_scan("scan: role grant",
    "GRANT `r1`@`%` TO `u`@`h`", false, false);
  check_scan("scan: REVOKE",
    "REVOKE SELECT ON `d`.* FROM `u`@`h`", false, false);

  check_scan_rows("scan rows: static option, mapped without it",
    {"GRANT SELECT ON *.* TO `u`@`h` WITH GRANT OPTION",
     "GRANT CONNECTION_ADMIN ON *.* TO `u`@`h`"}, true);
  check_scan_rows("scan rows: the other order",
    {"GRANT CONNECTION_ADMIN ON *.* TO `u`@`h`",
     "GRANT SELECT ON *.* TO `u`@`h` WITH GRANT OPTION"}, true);
  check_scan_rows("scan rows: static option, mapped with it",
    {"GRANT SELECT ON *.* TO `u`@`h` WITH GRANT OPTION",
     "GRANT CONNECTION_ADMIN ON *.* TO `u`@`h` WITH GRANT OPTION"}, false);
  check_scan_rows("scan rows: mapped without the option, static without it",
    {"GRANT SELECT ON *.* TO `u`@`h`",
     "GRANT CONNECTION_ADMIN ON *.* TO `u`@`h`"}, false);
}

static void check_revoke(const char *name, const char *revoke,
                         const char *role_name, const char *expected)
{
  std::string out= capture([&](FILE *f)
    { translate_mysql_revoke(revoke, f, role_name); });
  report(name, out == expected, out);
}

static void test_revoke()
{
  check_revoke("partial REVOKE is a DENY",
    "REVOKE INSERT ON `d\nx`.* FROM `u`@`h`", NULL,
    DENY_NEEDS_13_1_1(" ON `d x`.* FROM `u`@`h`")
    "/*M!130101 DENY INSERT ON `d\nx`.* TO `u`@`h` */;\n");

  check_revoke("role grantee replaced, unknown privilege dropped",
    "REVOKE INSERT, BACKUP_ADMIN ON `d`.* FROM `r`@`%`", "`r_0001`",
    "# WARNING: dropped MySQL privilege(s) BACKUP_ADMIN (no MariaDB "
    "equivalent): ON `d`.* FROM `r`@`%`, role `r_0001`\n"
    DENY_NEEDS_13_1_1(" ON `d`.* FROM `r`@`%`, role `r_0001`")
    "/*M!130101 DENY INSERT ON `d`.* TO `r_0001` */;\n");

  check_revoke("comment terminator in an identifier is plain",
    "REVOKE INSERT ON `d*/x`.* FROM `u`@`h`", NULL,
    DENY_NEEDS_13_1_1(" ON `d*/x`.* FROM `u`@`h`")
    NOGATE "DENY INSERT ON `d*/x`.* TO `u`@`h`;\n");

  check_revoke("role name containing a comment terminator is plain",
    "REVOKE INSERT ON `d`.* FROM `r`@`%`", "`r*/x`",
    DENY_NEEDS_13_1_1(" ON `d`.* FROM `r`@`%`, role `r*/x`")
    NOGATE "DENY INSERT ON `d`.* TO `r*/x`;\n");

  check_revoke("nothing left to DENY",
    "REVOKE BACKUP_ADMIN ON `d`.* FROM `u`@`h`", NULL,
    "# WARNING: dropped MySQL privilege(s) BACKUP_ADMIN (no MariaDB "
    "equivalent): ON `d`.* FROM `u`@`h`\n");

  check_revoke("malformed REVOKE is ignored", "REVOKE INSERT `d`.*", NULL,
    "# WARNING: unrecognized REVOKE ignored, the account is wider than "
    "on the MySQL source: REVOKE INSERT `d`.*\n");
}

static void collect_account(const char *user, const char *host, void *arg)
{
  *static_cast<std::string*>(arg)+= std::string(user) + "@" + host + ";";
}

static void check_account_list(const char *list, const char *expected)
{
  std::string got;
  parse_mysql_account_list(list, collect_account, &got);
  ok(got == expected, "account list [%s]: got [%s]", list, got.c_str());
}

static void test_account_list()
{
  check_account_list("", "");
  check_account_list("src_role_c", "src_role_c@%;");
  check_account_list("`a`@`h`,b@c", "a@h;b@c;");
  check_account_list(" a , b@h2 ", "a@%;b@h2;");
  check_account_list("'a''b'@'x', \"c\"", "a'b@x;c@%;");
  check_account_list("`a,b`@`h,1`,c", "a,b@h,1;c@%;");
  check_account_list("a,,", "a@%;");
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
    AUTH_LOCKED("sha256_password"));

  check_user("unsupported plugin without an ACCOUNT clause",
    " `u`@`h` IDENTIFIED WITH 'sha256_password' AS 'x'", false, false,
    "CREATE USER `u`@`h` ACCOUNT LOCK;\n"
    AUTH_LOCKED("sha256_password"));

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
    AUTH_LOCKED("bad DROP USER x; plug"));

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
    AUTH_LOCKED("sha256_password")
    "# WARNING: additional authentication factor 'caching_sha2_password' "
    "dropped: MariaDB cannot require it\n");

  check_user("DEFAULT ROLE is dropped",
    " `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' DEFAULT ROLE "
    "`r`@`%` REQUIRE NONE", false, false,
    "CREATE USER `u`@`h` IDENTIFIED WITH 'caching_sha2_password' AS 'h1' "
    "REQUIRE NONE;\n");
}

static void check_name(Mysql_roles &roles, const char *base, bool plain_ok,
                       const std::string &expected, const char *name)
{
  std::string got= roles.unique_name(base, plain_ok);
  ok(got == expected, "%s", name);
  if (got != expected)
    diag("got %s, expected %s", got.c_str(), expected.c_str());
}

static void test_role_names()
{
  Mysql_roles r;
  check_name(r, "foo", true, "foo", "a free name is used as it is");
  check_name(r, "foo", true, "foo_0001", "a taken name gets a suffix");
  check_name(r, "bar", false, "bar_0002", "a name with a host gets a suffix");
  check_name(r, "PUBLIC", true, "PUBLIC_0003", "PUBLIC is never used");
  check_name(r, "public", true, "public_0004", "public is never used");
  check_name(r, "None", true, "None_0005", "NONE is never used, any case");

  /* The counter skips a name that is taken */
  check_name(r, "a_0007", true, "a_0007", "a suffixed name can be free");
  check_name(r, "a", true, "a", "a is free");
  check_name(r, "a", true, "a_0006", "a is taken, the counter goes on");
  check_name(r, "a", true, "a_0008", "the counter skips a taken a_0007");

  check_name(r, "mariadb_dump_import_role", true,
             "mariadb_dump_import_role_0009", "the import role is taken");

  /* A long name is cut to 100 bytes, at a character boundary */
  check_name(r, std::string(150, 'x').c_str(), true,
             std::string(100, 'x') + "_0010", "a long name is cut");
  check_name(r, (std::string(99, 'x') + "\xC3\xA9" +
                 std::string(20, 'x')).c_str(),
             true, std::string(99, 'x') + "_0011",
             "a cut does not split a character");
  check_name(r, (std::string(100, 'x') + "\xC3\xA9").c_str(), true,
             std::string(100, 'x') + "_0012", "a cut at a boundary keeps it");
}

static void test_roles()
{
  Mysql_roles r;
  ok(std::string(r.quote_literal("a'b\\c*/d")) == "'a''b\\\\c*\\/d'",
     "a quote and a backslash are doubled, a comment end is escaped");
  ok(std::string(r.quote_account("u'", "h")) == "'u'''@'h'",
     "an account is quoted");

  r.add("u", "%", false, false);
  r.add("v", "h1", true, true);
  ok(r.count() == 2, "two roles added");
  const Mysql_role *u= r.find("u", "%");
  ok(u && !u->renamed && std::string(u->name) == "'u'" &&
     std::string(u->account) == "'u'@'%'" && !u->mandatory && !u->locked,
     "a role of host %% keeps the user name");
  const Mysql_role *v= r.find("v", "h1");
  ok(v && v->renamed && std::string(v->name) == "'v_0001'" &&
     std::string(v->account) == "'v'@'h1'" && v->mandatory && v->locked,
     "a role of another host is renamed");
  ok(!r.find("u", "h1"), "an account that is no role is not found");

  ok(std::string(r.default_roles_name("w")) == "'w_default_roles'",
     "the role of several default roles");
  ok(std::string(r.default_roles_name("w")) == "'w_default_roles_0002'",
     "it is renamed if taken");
}

static void test_emit()
{
  std::string out= capture([](FILE *f)
    { dump_emit_comment_warning(f, "a %s b", "x\ny\rz"); });
  ok(out == "# a x y z b\n", "CR/LF are folded in a comment warning");

  out= capture([](FILE *f)
    { dump_emit_comment_warning(f, "%s", std::string(3000, 'x').c_str()); });
  ok(out == "# " + std::string(3000, 'x') + "\n",
     "a long comment warning is not truncated");

  out= capture([](FILE *f)
    { dump_put_comment_safe_literal(f, "'a*/b*/'"); });
  ok(out == "'a*\\/b*\\/'", "terminators in a quoted literal are escaped");

  out= capture([](FILE *f)
    { dump_put_comment_safe_literal(f, "'ab'"); });
  ok(out == "'ab'", "literal without a terminator is unchanged");
}

[[noreturn]] void die(int error, const char *reason, ...)
{
  va_list args;
  va_start(args, reason);
  vfprintf(stderr, reason, args);
  va_end(args);
  fputc('\n', stderr);
  exit(error);
}

int main(int, char **argv)
{
  MY_INIT(argv[0]);
  plan(105);
  test_grant();
  test_scan();
  test_revoke();
  test_account_list();
  test_create_user();
  test_emit();
  test_role_names();
  test_roles();
  my_end(0);
  return exit_status();
}
