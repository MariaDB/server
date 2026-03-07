#ifndef COMMON_CLI_LONGOPTS_INCLUDED
#define COMMON_CLI_LONGOPTS_INCLUDED

  {"init-command", 0,
   "SQL Command to execute when connecting to MariaDB server. Will "
   "automatically be re-executed when reconnecting.",
   &opt_init_command, &opt_init_command, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

#endif /* COMMON_CLI_LONGOPTS_INCLUDED */
