//TODO license headr
#include <stdbool.h>
#include <tap.h>
#include <mysql.h>

int main()
{
  static const bool ARG_TRUE= true;
  static const char QUERY[]= "SELECT SUBSTRING_INDEX(USER(), '@', -1)";

  const char *const port_string= getenv("MASTER_MYPORT");
  const unsigned int port=
    port_string? (unsigned int)strtoul(port, NULL, 10) : 0;

  //TODO classes allow explicit case handling
  const char *bind_addresses[][2] = {
  { "127.0.0.1", /* Cloudflare DNS */ "1.0.0.1" },
  { "localhost", "example.com" },
  //TODO IPv6
};
  int count= sizeof(bind_addresses) / sizeof(const char *);

  plan(count);
  for (size_t i= 0; i < count; ++i)
  {
    const char *bind_address= bind_addresses[i];
    MYSQL connection;
    MYSQL_RES *results;
    bool success= false;

    // need init thread.  mysql_thread_init(); mysql_thread_end();
    if (!mysql_init(&connection))
    {
      diag("failed to initialize connection");
      goto end;
    }

    if (mysql_options(&connection, MYSQL_OPT_BIND, bind_address))
    {
      diag("MYSQL_OPT_BIND not accepted");
      goto close;
    }
    mysql_options(&connection, MYSQL_OPT_RECONNECT, &ARG_TRUE);

    if (!mysql_real_connect(&connection,
      NULL, // server address
      NULL, // user
      NULL, // password
      NULL, // database
      port,
      NULL, // socket
      0x0 // flags
    ))
    {
      if (/* TODO test fails from invalid addresses */ false)
        success= true;
      else
        diag("failed to connect to the server");
      goto close;
    }

    // TODO query

  close:
    mysql_close(&connection);
  end:
    ok(success, bind_address);
  }
  test_case();
  return exit_status();
}
