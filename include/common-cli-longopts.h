#ifndef COMMON_CLI_LONGOPTS_INCLUDED
#define COMMON_CLI_LONGOPTS_INCLUDED

  {"host", 'h',
   "Connect to host. Defaults in the following order: "
   "$MARIADB_HOST, $MYSQL_HOST, and then localhost",
   &opt_host, &opt_host, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"port", 'P', "Port number to use for connection or 0 for default to, in "
   "order of preference, my.cnf, $MYSQL_TCP_PORT, "
#if MYSQL_PORT_DEFAULT == 0
   "/etc/services, "
#endif
   "built-in default (" STRINGIFY_ARG(MYSQL_PORT) ").",
   &opt_mysql_port, &opt_mysql_port, 0, GET_UINT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"socket", 'S', "The socket file to use for connection.",
   &opt_mysql_unix_port, &opt_mysql_unix_port, 0, GET_STR_ALLOC,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"protocol", OPT_MYSQL_PROTOCOL,
   "The protocol to use for connection (tcp, socket, pipe).",
   &opt_protocol_type, &opt_protocol_type, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"compress", 0, "Use compression in server/client protocol.",
   &opt_compress, &opt_compress, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"plugin-dir", 0, "Directory for client-side plugins.",
   &opt_client_plugin_dir, &opt_client_plugin_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"plugin_dir", 0, "Alias for --plugin-dir.",
   &opt_client_plugin_dir, &opt_client_plugin_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"default-auth", 0, "Default authentication client-side plugin to use.",
   &opt_default_auth, &opt_default_auth, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"default_auth", 0, "Alias for --default-auth.",
   &opt_default_auth, &opt_default_auth, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"init-command", 0,
   "SQL Command to execute when connecting to MariaDB server. Will "
   "automatically be re-executed when reconnecting.",
   &opt_init_command, &opt_init_command, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#ifndef DONT_ALLOW_USER_CHANGE
  {"user", 'u', "User for login if not current user.",
   &opt_user, &opt_user, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif

#endif /* COMMON_CLI_LONGOPTS_INCLUDED */
