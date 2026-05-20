#ifndef COMMON_CLI_VARS_INCLUDED
#define COMMON_CLI_VARS_INCLUDED

/*
  Common CLI option variables and a helper to apply them to a MYSQL handle.
  These are intended to be shared across multiple client programs.
*/

/* Connection options */
static uint opt_mysql_port;
static char *opt_host, *opt_user, *opt_mysql_unix_port;

/* Other common options */
static my_bool opt_compress;
static uint opt_protocol;
static const char *opt_protocol_type = "";
static char *opt_init_command, *opt_client_plugin_dir, *opt_default_auth;

static inline void set_common_cli_host_from_env(void)
{
  // MARIADB_HOST will be preferred over MYSQL_HOST.
  if (!opt_host)
  {
    const char *tmp= getenv("MARIADB_HOST");
    if (!tmp)
      tmp= getenv("MYSQL_HOST");
    if (tmp)
      opt_host= my_strdup(PSI_NOT_INSTRUMENTED, tmp, MYF(MY_WME));
  }
}

static inline void set_common_cli_vars(MYSQL *mysql)
{
  if (opt_compress)
    mysql_options(mysql, MYSQL_OPT_COMPRESS, NullS);
  if (opt_protocol)
    mysql_options(mysql, MYSQL_OPT_PROTOCOL, (char *)&opt_protocol);
  if (opt_client_plugin_dir && *opt_client_plugin_dir)
    mysql_options(mysql, MYSQL_PLUGIN_DIR, opt_client_plugin_dir);
  if (opt_default_auth && *opt_default_auth)
    mysql_options(mysql, MYSQL_DEFAULT_AUTH, opt_default_auth);
}

static inline void set_common_cli_vars_with_init_command(MYSQL *mysql)
{
  set_common_cli_vars(mysql);
  if (opt_init_command && *opt_init_command)
    mysql_options(mysql, MYSQL_INIT_COMMAND, opt_init_command);
}

static inline void free_common_cli_vars(void)
{
  my_free(opt_host);
  my_free(opt_user);
  my_free(opt_mysql_unix_port);
}

#endif /* COMMON_CLI_VARS_INCLUDED */
