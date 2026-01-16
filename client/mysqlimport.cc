/*
   Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2011, 2024, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

/*
**	   mysqlimport.c  - Imports all given files
**			    into a table(s).
**
**			   *************************
**			   *			   *
**			   * AUTHOR: Monty & Jani  *
**			   * DATE:   June 24, 1997 *
**			   *			   *
**			   *************************
*/
#define VER "3.7"

#include "client_priv.h"
#include <my_sys.h>

#include "mysql_version.h"

#include <welcome_copyright_notice.h>   /* ORACLE_WELCOME_COPYRIGHT_NOTICE */

#include <string>
#include <fstream>
#include <sstream>
#include <tpool.h>
#include <vector>
#include <unordered_set>
#include <my_dir.h>
#include "import_util.h"

tpool::thread_pool *thread_pool;
static std::vector<MYSQL *> all_tp_connections;
std::atomic<bool> aborting{false};
static void kill_tp_connections(MYSQL *mysql);

static void db_error_with_table(MYSQL *mysql, char *table);
static void db_error(MYSQL *mysql);
static char *field_escape(char *to,const char *from,uint length);
static char *add_load_option(char *ptr,const char *object,
			     const char *statement);

static std::string parse_sql_script(const char *filepath, bool *tz_utc,
                                    std::vector<std::string> *trigger_defs);

static my_bool  verbose=0,lock_tables=0,ignore_errors=0,opt_delete=0,
                replace, silent, ignore, ignore_foreign_keys,
                opt_compress, opt_low_priority, tty_password;
static my_bool debug_info_flag= 0, debug_check_flag= 0;
static uint opt_use_threads=0, opt_local_file=0, my_end_arg= 0;
static char	*opt_password=0, *current_user=0,
		*current_host=0, *current_db=0, *fields_terminated=0,
		*lines_terminated=0, *enclosed=0, *opt_enclosed=0,
		*escaped=0, *opt_columns=0, 
		*default_charset= (char*) MYSQL_AUTODETECT_CHARSET_NAME;
static uint     opt_mysql_port= 0, opt_protocol= 0;
static char * opt_mysql_unix_port=0;
static char *opt_plugin_dir= 0, *opt_default_auth= 0;
static longlong opt_ignore_lines= -1;
static char *opt_dir;
static char opt_innodb_optimize_keys;

#include <sslopt-vars.h>

static char **argv_to_free;
static void safe_exit(int error, MYSQL *mysql);
static void set_exitcode(int code);

struct table_load_params
{
  std::string data_file; /* name of the file to load with LOAD DATA INFILE */
  std::string sql_file;  /* name of the file that contains CREATE TABLE or
                            CREATE VIEW */
  std::string dbname;    /* name of the database */
  bool tz_utc= false;    /* true if the script sets the timezone to UTC */
  bool is_view= false;   /* true if the script is for a VIEW */
  std::vector<std::string> triggers; /* CREATE TRIGGER statements */
  ulonglong size= 0;     /* size of the data file */
  std::string sql_text;  /* content of the SQL file, without triggers */
  TableDDLInfo ddl_info; /* parsed CREATE TABLE statement */

  table_load_params(const char* dfile, const char* sqlfile,
    const char* db, ulonglong data_size)
      : data_file(dfile), sql_file(sqlfile),
        dbname(db), triggers(),
        size(data_size),
        sql_text(parse_sql_script(sqlfile, &tz_utc, &triggers)),
        ddl_info(sql_text)
  {
    is_view= ddl_info.table_name.empty();
  }
  int create_table_or_view(MYSQL *);
  int load_data(MYSQL *);
};

std::unordered_set<std::string> ignore_databases;
std::unordered_set<std::string> ignore_tables;
std::unordered_set<std::string> include_databases;
std::unordered_set<std::string> include_tables;

static struct my_option my_long_options[] =
{
  {"character-sets-dir", 0,
   "Directory for character set files.", (char**) &charsets_dir,
   (char**) &charsets_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"database", OPT_DATABASE,
   "Restore the specified database, ignoring others.To specify more than one "
   "database to include, use the directive multiple times, once for each database. "
   "Only takes effect when used together with --dir option",
    0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"default-character-set", 0,
   "Set the default character set.", &default_charset,
   &default_charset, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"dir", 0, "Restore all tables from backup directory created using mariadb-dump --dir",
   &opt_dir, &opt_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"columns", 'c',
   "Use only these columns to import the data to. Give the column names in a comma separated list. This is same as giving columns to LOAD DATA INFILE.",
   &opt_columns, &opt_columns, 0, GET_STR, REQUIRED_ARG, 0, 0, 0,
   0, 0, 0},
  {"compress", 'C', "Use compression in server/client protocol.",
   &opt_compress, &opt_compress, 0, GET_BOOL, NO_ARG, 0, 0, 0,
   0, 0, 0},
  {"debug",'#', "Output debug log. Often this is 'd:t:o,filename'.", 0, 0, 0,
   GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"debug-check", 0, "Check memory and open file usage at exit.",
   &debug_check_flag, &debug_check_flag, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"debug-info", 0, "Print some debug info at exit.",
   &debug_info_flag, &debug_info_flag,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"default_auth", 0,
   "Default authentication client-side plugin to use.",
   &opt_default_auth, &opt_default_auth, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"delete", 'd', "First delete all rows from table.", &opt_delete,
   &opt_delete, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"fields-terminated-by", 0,
   "Fields in the input file are terminated by the given string.", 
   &fields_terminated, &fields_terminated, 0, 
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"fields-enclosed-by", 0,
   "Fields in the import file are enclosed by the given character.", 
   &enclosed, &enclosed, 0, 
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"fields-optionally-enclosed-by", 0,
   "Fields in the input file are optionally enclosed by the given character.", 
   &opt_enclosed, &opt_enclosed, 0, 
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"fields-escaped-by", 0, 
   "Fields in the input file are escaped by the given character.",
   &escaped, &escaped, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0,
   0, 0},
  {"force", 'f', "Continue even if we get an SQL error.",
   &ignore_errors, &ignore_errors, 0, GET_BOOL, NO_ARG, 0, 0,
   0, 0, 0, 0},
  {"help", '?', "Displays this help and exits.", 0, 0, 0, GET_NO_ARG, NO_ARG,
   0, 0, 0, 0, 0, 0},
  {"host", 'h', "Connect to host. Defaults in the following order: "
  "$MARIADB_HOST, and then localhost",
   &current_host, &current_host, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"ignore", 'i', "If duplicate unique key was found, keep old row.",
   &ignore, &ignore, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"ignore-foreign-keys", 'k',
    "Disable foreign key checks while importing the data.",
    &ignore_foreign_keys, &ignore_foreign_keys, 0, GET_BOOL, NO_ARG,
    0, 0, 0, 0, 0, 0},
  {"ignore-lines", 0, "Ignore first n lines of data infile.",
   &opt_ignore_lines, &opt_ignore_lines, 0, GET_LL,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"ignore-database", OPT_IGNORE_DATABASE,
   "Do not restore the specified database. To specify more than one database "
   "to ignore, use the directive multiple times, once for each database. Only "
   "takes effect when used together with --dir option",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"ignore-table", OPT_IGNORE_TABLE,
   "Do not restore the specified table. To specify more than one table to "
   "ignore, use the directive multiple times, once for each table.  Each "
   "table must be specified with both database and table names, e.g., "
   "--ignore-table=database.table.  Only takes effect when used together with "
   "--dir option",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
   {"innodb-optimize-keys", 0, "Create secondary indexes after data load (Innodb only).",
   &opt_innodb_optimize_keys, &opt_innodb_optimize_keys, 0, GET_BOOL, NO_ARG,
   1, 0, 0, 0, 0, 0},
  {"lines-terminated-by", 0, 
   "Lines in the input file are terminated by the given string.",
   &lines_terminated, &lines_terminated, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"local", 'L', "Read all files through the client.", &opt_local_file,
   &opt_local_file, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"lock-tables", 'l', "Lock all tables for write (this disables threads).",
    &lock_tables, &lock_tables, 0, GET_BOOL, NO_ARG, 
    0, 0, 0, 0, 0, 0},
  {"low-priority", 0,
   "Use LOW_PRIORITY when updating the table.", &opt_low_priority,
   &opt_low_priority, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"password", 'p',
   "Password to use when connecting to server. If password is not given it's asked from the tty.",
   0, 0, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
#ifdef _WIN32
  {"pipe", 'W', "Use named pipes to connect to server.", 0, 0, 0, GET_NO_ARG,
   NO_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"parallel", 'j', "Number of LOAD DATA jobs executed in parallel",
   &opt_use_threads, &opt_use_threads, 0, GET_UINT, REQUIRED_ARG, 0, 0, 0, 0,
   0, 0},
  {"plugin_dir", 0, "Directory for client-side plugins.",
   &opt_plugin_dir, &opt_plugin_dir, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"port", 'P', "Port number to use for connection or 0 for default to, in "
   "order of preference, my.cnf, $MYSQL_TCP_PORT, "
#if MYSQL_PORT_DEFAULT == 0
   "/etc/services, "
#endif
   "built-in default (" STRINGIFY_ARG(MYSQL_PORT) ").",
   &opt_mysql_port,
   &opt_mysql_port, 0, GET_UINT, REQUIRED_ARG, 0, 0, 0, 0, 0,
   0},
  {"protocol", OPT_MYSQL_PROTOCOL, "The protocol to use for connection (tcp, socket, pipe).",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"replace", 'r', "If duplicate unique key was found, replace old row.",
   &replace, &replace, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"silent", 's', "Be more silent.", &silent, &silent, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"socket", 'S', "The socket file to use for connection.",
   &opt_mysql_unix_port, &opt_mysql_unix_port, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"table", OPT_TABLES,
   "Restore the specified table ignoring others. Use --table=dbname.tablename with this option. "
   "To specify more than one table to include, use the directive multiple times, once for each "
   "table. Only takes effect when used together with --dir option",
    0, 0, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#include <sslopt-longopts.h>
  {"use-threads", 0, "Synonym for --parallel option",
   &opt_use_threads, &opt_use_threads, 0,
   GET_UINT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#ifndef DONT_ALLOW_USER_CHANGE
  {"user", 'u', "User for login if not current user.", &current_user,
   &current_user, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"verbose", 'v', "Print info about the various stages.", &verbose,
   &verbose, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Output version information and exit.", 0, 0, 0, GET_NO_ARG,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};


static const char *load_default_groups[]=
{ "mysqlimport", "mariadb-import", "client", "client-server", "client-mariadb",
  0 };


static void usage(void)
{
  puts("Copyright 2000-2008 MySQL AB, 2008 Sun Microsystems, Inc.");
  puts("Copyright 2008-2011 Oracle and Monty Program Ab.");
  puts("Copyright 2012-2019 MariaDB Corporation Ab.");
  print_version();
  puts(ORACLE_WELCOME_COPYRIGHT_NOTICE("2000"));
  printf("\
Loads tables from text files in various formats.  The base name of the\n\
text file must be the name of the table that should be used.\n\
If one uses sockets to connect to the MariaDB server, the server will open\n\
and read the text file directly. In other cases the client will open the text\n\
file. The SQL command 'LOAD DATA INFILE' is used to import the rows.\n");

  printf("\nUsage: %s [OPTIONS] database textfile...\n",my_progname);
  print_defaults("my",load_default_groups);
  puts("");
  my_print_help(my_long_options);
  my_print_variables(my_long_options);
}


static my_bool
get_one_option(const struct my_option *opt, const char *argument,
               const char *filename)
{
  switch(opt->id) {
  case 'p':
    if (argument == disabled_my_option)
      argument= (char*) "";			/* Don't require password */
    if (argument)
    {
      /*
        One should not really change the argument, but we make an
        exception for passwords
      */
      char *start= (char*) argument;
      my_free(opt_password);
      opt_password=my_strdup(PSI_NOT_INSTRUMENTED, argument,MYF(MY_FAE));
      while (*argument)
        *(char*) argument++= 'x';               /* Destroy argument */
      if (*start)
	start[1]=0;				/* Cut length of argument */
      tty_password= 0;
    }
    else
      tty_password= 1;
    break;
#ifdef _WIN32
  case 'W':
    opt_protocol = MYSQL_PROTOCOL_PIPE;
    break;
#endif
  case OPT_MYSQL_PROTOCOL:
    if ((opt_protocol= find_type_with_warning(argument, &sql_protocol_typelib,
                                              opt->name)) <= 0)
    {
      sf_leaking_memory= 1; /* no memory leak reports here */
      exit(1);
    }
    break;
  case 'P':
    if (filename[0] == '\0')
    {
      /* Port given on command line, switch protocol to use TCP */
      opt_protocol= MYSQL_PROTOCOL_TCP;
    }
    break;
  case 'S':
    if (filename[0] == '\0')
    {
      /*
        Socket given on command line, switch protocol to use SOCKETSt
        Except on Windows if 'protocol= pipe' has been provided in
        the config file or command line.
      */
      if (opt_protocol != MYSQL_PROTOCOL_PIPE)
      {
        opt_protocol= MYSQL_PROTOCOL_SOCKET;
      }
    }
    break;
  case (int) OPT_IGNORE_TABLE:
    if (!strchr(argument, '.'))
    {
      fprintf(stderr,
              "Illegal use of option --ignore-table=<database>.<table>\n");
      exit(1);
    }
    ignore_tables.insert(argument);
    break;
  case (int) OPT_TABLES:
    if (!strchr(argument, '.'))
    {
      fprintf(stderr,
             "Illegal use of option --table=<database>.<table>\n");
      exit(1);
    }
    include_tables.insert(argument);
    break;
  case (int) OPT_IGNORE_DATABASE:
    ignore_databases.insert(argument);
    break;
  case (int) OPT_DATABASE:
    include_databases.insert(argument);
    break;
  case '#':
    DBUG_PUSH(argument ? argument : "d:t:o");
    debug_check_flag= 1;
    break;
#include <sslopt-case.h>
  case 'V': print_version(); exit(0);
  case 'I':
  case '?':
    usage();
    exit(0);
  }
  return 0;
}


static int get_options(int *argc, char ***argv)
{
  int ho_error;

  if (current_host == NULL)
    current_host= getenv("MARIADB_HOST");

  if ((ho_error=handle_options(argc, argv, my_long_options, get_one_option)))
    exit(ho_error);
  if (debug_info_flag)
    my_end_arg= MY_CHECK_ERROR | MY_GIVE_INFO;
  if (debug_check_flag)
    my_end_arg= MY_CHECK_ERROR;

  if (enclosed && opt_enclosed)
  {
    fprintf(stderr, "You can't use ..enclosed.. and ..optionally-enclosed.. at the same time.\n");
    return(1);
  }
  if (replace && ignore)
  {
    fprintf(stderr, "You can't use --ignore (-i) and --replace (-r) at the same time.\n");
    return(1);
  }
  if (*argc < 2 && !opt_dir)
  {
    usage();
    return 1;
  }
  if (!opt_dir)
  {
    current_db= *((*argv)++);
    (*argc)--;
  }
  if (tty_password)
    opt_password=my_get_tty_password(NullS);
  return(0);
}

/**
  Check if file has given extension

  @param filename - name of the file
  @param ext - extension to check for, including the dot
  we assume that ext is always 4 characters long
*/
static bool has_extension(const char *filename, const char *ext)
{
  constexpr size_t ext_len= 4;
  DBUG_ASSERT(strlen(ext) == ext_len);
  size_t len= strlen(filename);
  return len >= ext_len && !strcmp(filename + len - ext_len, ext);
}

/**
  Quote an identifier, e.g table name, or dbname

  Adds ` around the string, and replaces with ` with `` inside the string
*/
static std::string quote_identifier(const char *name)
{
  std::string res;
  res.reserve(strlen(name)+2);
  res+= '`';
  for (const char *p= name; *p; p++)
  {
    if (*p == '`')
      res += '`';
    res += *p;
  }
  res += '`';
  return res;
}

/**
  Execute a batch of SQL statements

  @param mysql - connection to the server
  @param sql - SQL statements to execute, comma separated.
  @param filename - name of the file that contains the SQL statements

  @return 0 if successful, 1 if there was an error
*/
static int execute_sql_batch(MYSQL *mysql, const char *sql,
                             const char *filename)
{
  /* Execute batch */
  if (mysql_query(mysql, sql))
  {
    my_printf_error(0, "Error: %d, %s, when using script: %s", MYF(0),
                    mysql_errno(mysql), mysql_error(mysql), filename);
    safe_exit(1, mysql);
    return 1;
  }

  /* After we executed multi-statement batch, we need to read/check all
   * results. */
  for (int stmt_count= 1;; stmt_count++)
  {
    int res= mysql_next_result(mysql);
    switch (res)
    {
    case -1:
      return 0;
    case 0:
      break;
    default:
      my_printf_error(
          0, "Error: %d, %s, when using script: %s, statement count = %d",
          MYF(0), mysql_errno(mysql), mysql_error(mysql), filename,
          stmt_count + 1);
      safe_exit(1, mysql);
      return 1;
    }
  }
  return 0;
}

static int exec_sql(MYSQL *mysql, const std::string& s)
{
  if (mysql_query(mysql, s.c_str()))
  {
    if (!aborting)
    {
      fprintf(stdout, "Error: %d, %s, when using statement: %s\n",
              mysql_errno(mysql), mysql_error(mysql), s.c_str());
      db_error(mysql);
    }
    return 1;
  }
  return 0;
}

/**
  Prefix and suffix for CREATE TRIGGER statement in .sql file
*/
#define CREATE_TRIGGER_PREFIX "\nDELIMITER ;;\n"
#define CREATE_TRIGGER_SUFFIX ";;\nDELIMITER ;\n"

/**
  Parse the SQL script file, and return the content of the file as a string

  @param filepath - path to .sql file
  @param tz_utc - true if the script sets the timezone to UTC
  @param create_trigger_Defs  - will be filled with CREATE TRIGGER statements

  @return content of the file as a string, excluding CREATE TRIGGER statements
*/
static std::string parse_sql_script(const char *filepath, bool *tz_utc, std::vector<std::string> *create_trigger_Defs)
{
  /*Read full file to string*/
  std::ifstream t(filepath);
  std::stringstream sql;
  sql << t.rdbuf();

  std::string sql_text= sql.str();

  /*
    This is how triggers are defined in .sql file by mysqldump

    DELIMITER ;;
    CREATE TRIGGER <some statements>;;
    DELIMITER ;
    Now, DELIMITER is not a statement, but a command for the mysql client.
    Thus we can't sent it as part of the batch, so we transform the above
    by removing DELIMITER lines, and extra semicolon at the end of the
    CREATE TRIGGER statement.
  */
  for (;;)
  {
    auto pos= sql_text.find(CREATE_TRIGGER_PREFIX);
    if (pos == std::string::npos)
      break;
    auto end_pos= sql_text.find(CREATE_TRIGGER_SUFFIX, pos);
    if (end_pos == std::string::npos)
      break;
    create_trigger_Defs->push_back(sql_text.substr(pos + sizeof(CREATE_TRIGGER_PREFIX)-1,
        end_pos - pos - sizeof(CREATE_TRIGGER_PREFIX) +1));
    sql_text.erase(pos, end_pos - pos + sizeof(CREATE_TRIGGER_SUFFIX) - 1);
  }

  /*
   Find out if dump was made using UTC timezone, we'd need the same for the
   loading. output in UTC timezone is default in mysqldump, but can be controlled
   with --tz-utc option
  */
  *tz_utc= sql_text.find("SET TIME_ZONE='+00:00'") != std::string::npos;
  return sql_text;
}

/*
  Creates database if it does not yet exists.

  @param mysql - connection to the server
  @param dbname - name of the database
*/
static int create_db_if_not_exists(MYSQL *mysql, const char *dbname)
{
  /* Create database if it does not yet exist */
  std::string create_db_if_not_exists= "CREATE DATABASE IF NOT EXISTS ";
  create_db_if_not_exists+= quote_identifier(dbname);
  if (mysql_query(mysql, create_db_if_not_exists.c_str()))
  {
    db_error(mysql);
    return 1;
  }
  return 0;
}


#include "import_util.h"

int table_load_params::create_table_or_view(MYSQL* mysql)
{
  if (sql_file.empty())
    return 0;

  if (verbose && !sql_file.empty())
  {
    fprintf(stdout, "Executing SQL script %s\n", sql_file.c_str());
  }
  if (!dbname.empty())
  {
    if (mysql_select_db(mysql, dbname.c_str()))
    {
      if (create_db_if_not_exists(mysql, dbname.c_str()))
        return 1;
      if (mysql_select_db(mysql, dbname.c_str()))
      {
        db_error(mysql);
        return 1;
      }
    }
  }
  if (sql_text.empty())
  {
    fprintf(stderr, "Error: CREATE TABLE statement not found in %s\n",
              sql_file.c_str());
    return 1;
  }
  if (execute_sql_batch(mysql, sql_text.c_str(), sql_file.c_str()))
    return 1;
  /*
  Temporarily drop from table definitions, if --innodb-optimize-keys is given. We'll add them back
  later, after the data is loaded.
*/
  auto drop_constraints_sql= ddl_info.drop_constraints_sql();
  if (!drop_constraints_sql.empty())
  {
    if (exec_sql(mysql, drop_constraints_sql))
      return 1;
  }
  return 0;
}

int table_load_params::load_data(MYSQL *mysql)
{
  char tablename[FN_REFLEN], hard_path[FN_REFLEN],
       escaped_name[FN_REFLEN * 2 + 1],
       sql_statement[FN_REFLEN*16+256], *end;
  DBUG_ENTER("table_load_params::load");
  DBUG_PRINT("enter",("datafile: %s",data_file.c_str()));
  DBUG_ASSERT(!dbname.empty());

  if (data_file.empty())
    DBUG_RETURN(0);

  if (aborting)
    DBUG_RETURN(0);

  if (!dbname.empty() && mysql_select_db(mysql, dbname.c_str()))
  {
    db_error(mysql);
    DBUG_RETURN(1);
  }

  const char *filename= data_file.c_str();

  fn_format(tablename, filename, "", "", MYF(MY_REPLACE_DIR | MY_REPLACE_EXT));
  if (strchr(tablename, '@'))
  {
    uint errors, len;
    CHARSET_INFO *cs=
        get_charset_by_csname(default_charset, MY_CS_PRIMARY, MYF(0));
    len= my_convert(escaped_name, sizeof(escaped_name) - 1, cs, tablename,
                    (uint32)strlen(tablename), &my_charset_filename, &errors);
    if (!errors)
      strmake(tablename, escaped_name, len);
  }

  const char *db= current_db ? current_db : dbname.c_str();
  std::string full_tablename= quote_identifier(db);
  full_tablename+= ".";
  full_tablename+= quote_identifier(tablename);

  if (tz_utc && exec_sql(mysql, "SET TIME_ZONE='+00:00';"))
    DBUG_RETURN(1);
  if (exec_sql(mysql,
               std::string("ALTER TABLE ") + full_tablename + " DISABLE KEYS"))
   DBUG_RETURN(1);

  if (!opt_local_file)
    strmov(hard_path,filename);
  else
    my_load_path(hard_path, filename, NULL); /* filename includes the path */

  if (opt_delete)
  {
    if (verbose)
      fprintf(stdout, "Deleting the old data from table %s\n", tablename);
    snprintf(sql_statement, FN_REFLEN * 16 + 256, "DELETE FROM %s",
             full_tablename.c_str());
    if (exec_sql(mysql, sql_statement))
      DBUG_RETURN(1);
  }


  bool recreate_secondary_keys= false;
  if (opt_innodb_optimize_keys && ddl_info.storage_engine == "InnoDB")
  {
    auto drop_secondary_keys_sql= ddl_info.drop_secondary_indexes_sql();
    if (!drop_secondary_keys_sql.empty())
    {
      recreate_secondary_keys= true;
      if (exec_sql(mysql, drop_secondary_keys_sql))
        DBUG_RETURN(1);
    }
  }
  if (exec_sql(mysql, "SET collation_database=binary"))
    DBUG_RETURN(1);
  to_unix_path(hard_path);
  if (verbose)
  {
    fprintf(stdout, "Loading data from %s file: %s into %s\n",
           (opt_local_file) ? "LOCAL" : "SERVER", hard_path, tablename);
  }
  mysql_real_escape_string(mysql, escaped_name, hard_path,
                           (unsigned long) strlen(hard_path));
  snprintf(sql_statement, sizeof(sql_statement),
          "LOAD DATA %s %s INFILE '%s'",
          opt_low_priority ? "LOW_PRIORITY" : "",
          opt_local_file ? "LOCAL" : "", escaped_name);

  end= strend(sql_statement);
  if (replace)
    end= strmov(end, " REPLACE");
  if (ignore)
    end= strmov(end, " IGNORE");
  end= strmov(end, " INTO TABLE ");

  end= strmov(end,full_tablename.c_str());

  if (fields_terminated || enclosed || opt_enclosed || escaped)
      end= strmov(end, " FIELDS");
  end= add_load_option(end, fields_terminated, " TERMINATED BY");
  end= add_load_option(end, enclosed, " ENCLOSED BY");
  end= add_load_option(end, opt_enclosed,
		       " OPTIONALLY ENCLOSED BY");
  end= add_load_option(end, escaped, " ESCAPED BY");
  end= add_load_option(end, lines_terminated, " LINES TERMINATED BY");
  if (opt_ignore_lines >= 0)
    end= strmov(longlong10_to_str(opt_ignore_lines, 
				  strmov(end, " IGNORE "),10), " LINES");
  if (opt_columns)
    end= strmov(strmov(strmov(end, " ("), opt_columns), ")");
  *end= '\0';

  if (mysql_query(mysql, sql_statement))
  {
    db_error_with_table(mysql, tablename);
    DBUG_RETURN(1);
  }
  if (!silent)
  {
    const char *info= mysql_info(mysql);
    if (info) /* If NULL-pointer, print nothing */
      fprintf(stdout, "%s.%s: %s\n", db, tablename, info);
  }


  if (exec_sql(mysql, std::string("ALTER TABLE ") + full_tablename + " ENABLE KEYS;"))
    DBUG_RETURN(1);

  if (ddl_info.storage_engine == "MyISAM" || ddl_info.storage_engine == "Aria")
  {
    /* Avoid "table was not properly closed" warnings */
    if (exec_sql(mysql, std::string("FLUSH TABLE ").append(full_tablename).c_str()))
      DBUG_RETURN(1);
  }
  if (recreate_secondary_keys)
  {
    auto create_secondary_keys_sql= ddl_info.add_secondary_indexes_sql();
    if (!create_secondary_keys_sql.empty())
    {
      if (verbose)
      {
        fprintf(stdout, "Adding secondary indexes to table %s\n", ddl_info.table_name.c_str());
      }
      if (exec_sql(mysql, create_secondary_keys_sql))
        DBUG_RETURN(1);
    }
  }
  if (tz_utc)
  {
    if (exec_sql(mysql, "SET TIME_ZONE=@save_tz;"))
      DBUG_RETURN(1);
  }
  /* Restore constraints and triggers */
  for (const auto &create_trigger_def : triggers)
  {
    if (exec_sql(mysql, create_trigger_def))
      return 1;
  }

  if (opt_innodb_optimize_keys && ddl_info.storage_engine == "InnoDB")
  {
    std::string constraints= ddl_info.add_constraints_sql();
    if (!constraints.empty())
    {
      if (exec_sql(mysql, constraints))
        return 1;
    }
  }
  DBUG_RETURN(0);
}



static void lock_table(MYSQL *mysql, int tablecount, char **raw_tablename)
{
  DYNAMIC_STRING query;
  int i;
  char tablename[FN_REFLEN];

  if (verbose)
    fprintf(stdout, "Locking tables for write\n");
  init_dynamic_string(&query, "LOCK TABLES ", 256, 1024);
  for (i=0 ; i < tablecount ; i++)
  {
    fn_format(tablename, raw_tablename[i], "", "", 1 | 2);
    dynstr_append(&query, tablename);
    dynstr_append(&query, " WRITE,");
  }
  if (mysql_real_query(mysql, query.str, (ulong)query.length-1))
    db_error(mysql); /* We shall continue here, if --force was given */
}


/*
  Check server version, and return true if bulk load can be enabled
  Works around MDEV-34703 (fixed in 10.11.11, 11.4.5, 11.7.2)
*/
static bool can_enable_innodb_bulk_load(MYSQL* mysql)
{
  auto ver = mysql_get_server_version(mysql);
  return ver >= 110702 ||
      (ver >= 110405 && ver < 110500) ||
      (ver >= 101111 && ver < 101200);
}

static MYSQL *db_connect(char *host, char *database,
                         char *user, char *passwd)
{
  MYSQL *mysql;
  my_bool reconnect;
  if (verbose)
    fprintf(stdout, "Connecting to %s\n", host ? host : "localhost");
  if (opt_use_threads && !lock_tables)
  {
    if (!(mysql= mysql_init(NULL)))
    {
      return 0;
    }
  }
  else
    if (!(mysql= mysql_init(NULL)))
      return 0;
  if (opt_compress)
    mysql_options(mysql,MYSQL_OPT_COMPRESS,NullS);
  if (opt_local_file)
    mysql_options(mysql,MYSQL_OPT_LOCAL_INFILE,
		  (char*) &opt_local_file);
  SET_SSL_OPTS(mysql);
  if (opt_protocol)
    mysql_options(mysql,MYSQL_OPT_PROTOCOL,(char*)&opt_protocol);

  if (opt_plugin_dir && *opt_plugin_dir)
    mysql_options(mysql, MYSQL_PLUGIN_DIR, opt_plugin_dir);

  if (opt_default_auth && *opt_default_auth)
    mysql_options(mysql, MYSQL_DEFAULT_AUTH, opt_default_auth);
  if (!strcmp(default_charset,MYSQL_AUTODETECT_CHARSET_NAME))
    default_charset= (char *)my_default_csname();
  my_set_console_cp(default_charset);
  mysql_options(mysql, MYSQL_SET_CHARSET_NAME, default_charset);
  mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_RESET, 0);
  mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD,
                 "program_name", "mysqlimport");
  if (!(mysql_real_connect(mysql,host,user,passwd,
                           database,opt_mysql_port,opt_mysql_unix_port,
                           opt_dir?CLIENT_MULTI_STATEMENTS:0)))
  {
    ignore_errors=0;	  /* NO RETURN FROM db_error */
    db_error(mysql);
  }
  reconnect= 0;
  mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect);
  if (database)
  {
    if (verbose)
      fprintf(stdout, "Selecting database %s\n", database);
    if (mysql_select_db(mysql, database))
    {
      ignore_errors= 0;
      db_error(mysql);
    }
  }
  if (ignore_foreign_keys)
    mysql_query(mysql, "set foreign_key_checks= 0;");

  if (can_enable_innodb_bulk_load(mysql))
  {
    if (mysql_query(mysql, "set unique_checks=0;"))
      db_error(mysql);
  }
  if (mysql_query(mysql, "/*!40101 set @@character_set_database=binary */;"))
    db_error(mysql);
  if (mysql_query(mysql, "set @save_tz=@@session.time_zone"))
    db_error(mysql);
  if (mysql_query(mysql, "/*M!100200 set check_constraint_checks=0*/"))
    db_error(mysql);
  return mysql;
}



static void db_disconnect(char *host, MYSQL *mysql)
{
  if (verbose)
    fprintf(stdout, "Disconnecting from %s\n", host ? host : "localhost");
  mysql_close(mysql);
}


static void safe_exit(int error, MYSQL *mysql)
{

  if (error && ignore_errors)
    return;

  bool expected= false;
  if (!aborting.compare_exchange_strong(expected, true))
    return;

  if (thread_pool)
  {
    /* dirty exit. some threads are running,
    memory is not freed, openssl not deinitialized */
    DBUG_ASSERT(error);
    if (mysql)
    {
      /*
        We still need tell server to kill all connections
        so it does not keep busy with load.
      */
      kill_tp_connections(mysql);
    }

    _exit(error);
  }
  if (mysql)
  {
    mysql_close(mysql);
  }
  mysql_library_end();
  free_defaults(argv_to_free);
  my_free(opt_password);
  my_end(my_end_arg); /* clean exit */
  exit(error);
}



static void db_error_with_table(MYSQL *mysql, char *table)
{
  if (aborting)
    return;
  my_printf_error(0,"Error: %d, %s, when using table: %s",
		  MYF(0), mysql_errno(mysql), mysql_error(mysql), table);
  safe_exit(1, mysql);
}

static void fatal_error(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fflush(stderr);
  ignore_errors= 0;
  safe_exit(1, 0);
}


static void db_error(MYSQL *mysql)
{
  if (aborting)
    return;
  const char *info= mysql_info(mysql);
  auto err= mysql_errno(mysql);
  auto err_text = mysql_error(mysql);
  if (info)
    my_printf_error(0,"Error: %d %s %s", MYF(0), err, err_text, info);
  else
    my_printf_error(0, "Error %d %s", MYF(0), err, err_text);
  safe_exit(1, mysql);
}


static char *add_load_option(char *ptr, const char *object,
			     const char *statement)
{
  if (object)
  {
    /* Don't escape hex constants */
    if (object[0] == '0' && (object[1] == 'x' || object[1] == 'X'))
      ptr= strxmov(ptr," ",statement," ",object,NullS);
    else
    {
      /* char constant; escape */
      ptr= strxmov(ptr," ",statement," '",NullS);
      ptr= field_escape(ptr,object,(uint) strlen(object));
      *ptr++= '\'';
    }
  }
  return ptr;
}

/*
** Allow the user to specify field terminator strings like:
** "'", "\", "\\" (escaped backslash), "\t" (tab), "\n" (newline)
** This is done by doubling ' and add a end -\ if needed to avoid
** syntax errors from the SQL parser.
*/ 

static char *field_escape(char *to,const char *from,uint length)
{
  const char *end;
  uint end_backslashes=0; 

  for (end= from+length; from != end; from++)
  {
    *to++= *from;
    if (*from == '\\')
      end_backslashes^=1;    /* find odd number of backslashes */
    else 
    {
      if (*from == '\'' && !end_backslashes)
	*to++= *from;      /* We want a duplicate of "'" for MySQL */
      end_backslashes=0;
    }
  }
  /* Add missing backslashes if user has specified odd number of backs.*/
  if (end_backslashes)
    *to++= '\\';          
  return to;
}

std::atomic<int> exitcode;
void set_exitcode(int code)
{
  int expected= 0;
  exitcode.compare_exchange_strong(expected,code);
}

static thread_local MYSQL *thread_local_mysql;


static void load_single_table(void *arg)
{
  int error;
  table_load_params *params= (table_load_params *) arg;
  if ((error= params->load_data(thread_local_mysql)))
    set_exitcode(error);
}

static void init_tp_connections(size_t n)
{
  for (size_t i= 0; i < n; i++)
  {
    MYSQL *mysql=
        db_connect(current_host, current_db, current_user, opt_password);
    all_tp_connections.push_back(mysql);
  }
}

static void close_tp_connections()
{
  for (auto &conn : all_tp_connections)
  {
    db_disconnect(current_host, conn);
  }
  all_tp_connections.clear();
}

/*
  If we end with an error, in one connection,
  we need to kill all others.

  Otherwise, server will still be busy with load,
  when we already exited.
*/
static void kill_tp_connections(MYSQL *mysql)
{
  for (auto &conn : all_tp_connections)
    mysql_kill(mysql, mysql_thread_id(conn));
}

static void tpool_thread_init(void)
{
  mysql_thread_init();
  static std::atomic<size_t> next_connection(0);
  assert(next_connection < all_tp_connections.size());
  thread_local_mysql= all_tp_connections[next_connection++];
}

static void tpool_thread_exit(void)
{
  mysql_thread_end();
}

/**
  Get files to load, for --dir case
  Enumerates all files in the subdirectories, and returns only *.txt files
  (table data files),  or .sql files,  there is no corresponding .txt file
  (view definitions)

  @param dir - directory to scan
  @param files - vector to store the files

  @note files are sorted by size, descending
*/
static void scan_backup_dir(const char *dir,
                            std::vector<table_load_params> &files)
{
  MY_DIR *dir_info;
  std::vector<std::string> subdirs;
  int stat_err;
  struct stat st;
  if ((stat_err= stat(dir, &st)) != 0 || (st.st_mode & S_IFDIR) == 0)
  {
    fatal_error("%s: Path '%s' specified by option '--dir' %s\n",
                my_progname_short, dir,
                stat_err ? "does not exist" : "is not a directory");
  }
  dir_info= my_dir(dir, MYF(MY_DONT_SORT | MY_WANT_STAT | MY_WME));
  if (!dir_info)
  {
    fatal_error("Can't read directory '%s', error %d", opt_dir, errno);
    return;
  }
  for (size_t i= 0; i < dir_info->number_of_files; i++)
  {
    const fileinfo *fi= &dir_info->dir_entry[i];
    if (!(fi->mystat->st_mode & S_IFDIR))
      continue;
    if (!strcmp(fi->name, ".") || !strcmp(fi->name, ".."))
      continue;
    if (ignore_databases.find(fi->name) != ignore_databases.end())
      continue;
    if (include_databases.size() &&
        include_databases.find(fi->name) == include_databases.end())
      continue;
    std::string subdir= dir;
    subdir+= "/";
    subdir+= fi->name;
    const char *dbname = fi->name;
    // subdirs.push_back(subdir);
    MY_DIR *dir_info2=
        my_dir(subdir.c_str(), MYF(MY_DONT_SORT | MY_WANT_STAT | MY_WME));
    if (!dir_info2)
    {
      fatal_error("Can't read directory %s , error %d", subdir.c_str(), errno);
      return;
    }
    for (size_t j= 0; j < dir_info2->number_of_files; j++)
    {
      fi= &dir_info2->dir_entry[j];
      if (has_extension(fi->name, ".sql") || has_extension(fi->name, ".txt"))
      {
        std::string full_table_name=
            std::string(dbname) + "." + std::string(fi->name);
        full_table_name.resize(full_table_name.size() - 4);
        if (ignore_tables.find(full_table_name) != ignore_tables.end())
        {
          continue;
        }
        if (include_tables.size() &&
            include_tables.find(full_table_name) == include_tables.end())
        {
           continue;
        }
      }

      std::string file= subdir;
      file+= "/";
      file+= fi->name;
      DBUG_ASSERT(access(file.c_str(), F_OK) == 0);
      if (!MY_S_ISDIR(fi->mystat->st_mode))
      {
        /* test file*/
        if (has_extension(fi->name, ".txt"))
        {
          auto sql_file= file.substr(0, file.size() - 4) + ".sql";
          if (access(sql_file.c_str(), F_OK))
          {
            fatal_error("Expected file '%s' is missing",sql_file.c_str());
          }
          table_load_params par(file.c_str(), sql_file.c_str(), dbname,
                                fi->mystat->st_size);

          files.push_back(par);
        }
        else if (has_extension(fi->name, ".sql"))
        {
          /*
            Check whether it is a view definition, without a
            corresponding .txt file, add .sql file then
          */
          std::string txt_file= file.substr(0, file.size() - 4) + ".txt";
          if (access(txt_file.c_str(), F_OK))
          {
            table_load_params par("", file.c_str(), dbname,
                                  fi->mystat->st_size);
            files.push_back(par);
          }
        }
        else
        {
          fatal_error("Unexpected file '%s' in directory '%s'", fi->name,subdir.c_str());
        }
      }
    }
    my_dirend(dir_info2);
  }
  my_dirend(dir_info);

  /* sort files by size, descending. Put view definitions at the end of the list.*/
  std::sort(files.begin(), files.end(),
            [](const table_load_params &a, const table_load_params &b) -> bool
            {
               /* Sort views after base tables */
               if (a.is_view && !b.is_view)
                 return false;
               if (!a.is_view && b.is_view)
                 return true;
               /* Sort by size descending */
              if (a.size > b.size)
                  return true;
              if (a.size < b.size)
                  return false;
              /* If sizes are equal, sort by name */
              return a.sql_file < b.sql_file;
            });
}

#define MAX_THREADS 256

int main(int argc, char **argv)
{
  int error=0;
  MY_INIT(argv[0]);
  sf_leaking_memory=1; /* don't report memory leaks on early exits */

  /* We need to know if protocol-related options originate from CLI args */
  my_defaults_mark_files = TRUE;

  load_defaults_or_exit("my", load_default_groups, &argc, &argv);
  /* argv is changed in the program */
  argv_to_free= argv;
  if (get_options(&argc, &argv))
  {
    free_defaults(argv_to_free);
    return(1);
  }
  if (opt_use_threads > MAX_THREADS)
  {
    fatal_error("Too many connections, max value for --parallel is %d\n",
                MAX_THREADS);
  }
  sf_leaking_memory=0; /* from now on we cleanup properly */

  std::vector<table_load_params> files_to_load;

  if (opt_dir)
  {
    ignore_foreign_keys= 1;
    if (argc)
      fatal_error("Invalid arguments for --dir option");
    scan_backup_dir(opt_dir, files_to_load);
  }
  else
  {
    for (; *argv != NULL; argv++)
    {
      table_load_params p(*argv, "", current_db, 0);
      files_to_load.push_back(p);
    }
  }
  if (files_to_load.empty())
  {
    fatal_error("No files to load");
    return 1;
  }
  MYSQL *mysql=
      db_connect(current_host, current_db, current_user, opt_password);
  if (!mysql)
  {
    free_defaults(argv_to_free);
    return 1;
  }
  for (auto &f : files_to_load)
  {
    if (f.create_table_or_view(mysql))
      set_exitcode(1);
  }

  if (opt_use_threads && !lock_tables)
  {
    init_tp_connections(opt_use_threads);
    thread_pool= tpool::create_thread_pool_generic(opt_use_threads,opt_use_threads);
    thread_pool->set_thread_callbacks(tpool_thread_init,tpool_thread_exit);

    std::vector<tpool::task> load_tasks;
    for (const auto &f : files_to_load)
    {
      load_tasks.push_back(tpool::task(load_single_table, (void *) &f));
    }

    for (auto &t: load_tasks)
      thread_pool->submit_task(&t);

    delete thread_pool;

    close_tp_connections();
    thread_pool= nullptr;
  }
  else
  {
    if (lock_tables)
      lock_table(mysql, argc, argv);
    for (auto &f : files_to_load)
    {
      if ((error= f.load_data(mysql)))
        set_exitcode(error);
    }
  }
  mysql_close(mysql);
  safe_exit(0, 0);
  return(exitcode);
}
