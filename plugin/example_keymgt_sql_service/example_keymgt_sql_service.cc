#include <mysql/plugin_encryption.h>
#include <cstring>

#define STRING_WITH_LEN(X) (X ""), ((size_t) (sizeof(X) - 1))

/* my_base.h without compile errors */
#define HA_ERR_RETRY_INIT 129

// AES128-GCM 128-bit key
#define KEY_LEN 16

static int run_sql(const char *query, size_t query_len)
{
  MYSQL_RES *res;

  MYSQL *mysql= mysql_init(NULL);
  if (!mysql)
    return 1;

  if (mysql_real_connect_local(mysql) == NULL)
    return 1;

  if (mysql_real_query(mysql, query, query_len))
    return 1;

  if (!(res= mysql_store_result(mysql)))
      return 1;

  mysql_free_result(res);


  mysql_close(mysql);
  return 0;
}

static unsigned int
get_latest_key_version(unsigned int key_id)
{
  return run_sql(STRING_WITH_LEN("SELECT * FROM test.t1"));
}

static unsigned int
get_key(unsigned int key_id, unsigned int version,
        unsigned char* dstbuf, unsigned *buflen)
{

  if (run_sql(STRING_WITH_LEN("SELECT * FROM test.t1")))
    return 1;

  if (dstbuf == NULL) {
    *buflen = KEY_LEN;
  } else {
    memset(dstbuf, 9, *buflen);
  }

  return 0;
}

static int example_keymgt_sql_service_init(void *p)
{
  run_sql(STRING_WITH_LEN("CREATE TABLE test.t1 (id int)")); // ? HA_ERR_RETRY_INIT : 0;
  return 0;
}

static int example_keymgt_sql_service_deinit(void *p)
{
  /* MDEV-33047 using sql_service within deinit of plugin segfaults on shutdown
  return run_sql(STRING_WITH_LEN("DROP TABLE test.t1"));
  */
  return 0;
}

struct st_mariadb_encryption example_keymgt_sql_service= {
  MariaDB_ENCRYPTION_INTERFACE_VERSION,
  get_latest_key_version,
  get_key,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
};

/*
  Plugin library descriptor
*/
maria_declare_plugin(example_keymgt_sql_service)
{
  MariaDB_ENCRYPTION_PLUGIN,
  &example_keymgt_sql_service,
  "example_keymgt_sql_service",
  "Trevor, Daniel",
  "Example keymgt plugin that uses sql service",
  PLUGIN_LICENSE_GPL,
  example_keymgt_sql_service_init,
  example_keymgt_sql_service_deinit,
  0x0100 /* 1.0 */,
  NULL,	/* status variables */
  NULL,	/* system variables */
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
