#include <mysql.h>
#include <stdlib.h>
#include <cstdio>

static const char *server_args[]= {"this_program", /* this string is not used */
                             "--datadir=.", "--key_buffer_size=32M"};
static const char *server_groups[]= {"embedded", "server", "this_program_SERVER",
                               (char *) NULL};

int main(void)
{
  if (mysql_library_init(sizeof(server_args) / sizeof(char *), (char **)server_args,
                         NULL))
  {
    fprintf(stderr, "could not initialize MySQL client library\n");
    exit(1);
  }

  /* Use any MySQL API functions here */

  mysql_library_end();

  mysql_library_init(sizeof(server_args) / sizeof(char *), (char **)server_args, NULL);

  mysql_library_end();

  return EXIT_SUCCESS;
}
