/*
  Thin C API around libmariadbd for the WASM / JS package.
  Returns malloc'd JSON; the JS wrapper frees it via l4m_free().
*/

#include <mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

static MYSQL *g_mysql;
static int g_open;

static void ensure_dir(const char *path)
{
  if (mkdir(path, 0777) != 0 && errno != EEXIST)
  {
    /* best-effort; mysql_server_init will report a real error */
  }
}

typedef struct
{
  char *p;
  size_t n;
  size_t cap;
} buf_t;

static int buf_reserve(buf_t *b, size_t extra)
{
  if (b->n + extra + 1 <= b->cap)
    return 0;
  size_t ncap= b->cap ? b->cap * 2 : 512;
  while (ncap < b->n + extra + 1)
    ncap *= 2;
  char *np= (char *)realloc(b->p, ncap);
  if (!np)
    return -1;
  b->p= np;
  b->cap= ncap;
  return 0;
}

static int buf_putc(buf_t *b, char c)
{
  if (buf_reserve(b, 1))
    return -1;
  b->p[b->n++]= c;
  b->p[b->n]= 0;
  return 0;
}

static int buf_puts(buf_t *b, const char *s)
{
  size_t len= strlen(s);
  if (buf_reserve(b, len))
    return -1;
  memcpy(b->p + b->n, s, len);
  b->n += len;
  b->p[b->n]= 0;
  return 0;
}

static int buf_put_json_hex(buf_t *b, const char *s, unsigned long len)
{
  static const char hexd[]= "0123456789abcdef";
  if (buf_putc(b, '"'))
    return -1;
  for (unsigned long i= 0; i < len; i++)
  {
    unsigned char c= (unsigned char)s[i];
    if (buf_putc(b, hexd[c >> 4]) || buf_putc(b, hexd[c & 0xf]))
      return -1;
  }
  return buf_putc(b, '"');
}

static int is_binary_field(const MYSQL_FIELD *f)
{
  if (!f)
    return 0;
  switch (f->type)
  {
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
    /* TEXT/JSON share the BLOB type code; only binary charset is binary. */
    return f->charsetnr == 63;
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_VAR_STRING:
#ifdef MYSQL_TYPE_VARCHAR
  case MYSQL_TYPE_VARCHAR:
#endif
    return f->charsetnr == 63;
  default:
    return 0;
  }
}

static int buf_put_json_str(buf_t *b, const char *s)
{
  if (buf_putc(b, '"'))
    return -1;
  for (; *s; s++)
  {
    unsigned char c= (unsigned char)*s;
    if (c == '"' || c == '\\')
    {
      if (buf_putc(b, '\\') || buf_putc(b, (char)c))
        return -1;
    }
    else if (c == '\n')
    {
      if (buf_puts(b, "\\n"))
        return -1;
    }
    else if (c == '\r')
    {
      if (buf_puts(b, "\\r"))
        return -1;
    }
    else if (c == '\t')
    {
      if (buf_puts(b, "\\t"))
        return -1;
    }
    else if (c < 0x20)
    {
      char tmp[8];
      snprintf(tmp, sizeof(tmp), "\\u%04x", c);
      if (buf_puts(b, tmp))
        return -1;
    }
    else if (buf_putc(b, (char)c))
      return -1;
  }
  return buf_putc(b, '"');
}

static char *dup_str(const char *s)
{
  size_t n= strlen(s) + 1;
  char *p= (char *)malloc(n);
  if (p)
    memcpy(p, s, n);
  return p;
}

static char *json_err_state(int err, const char *msg, const char *sqlstate)
{
  buf_t b= {0};
  if (buf_puts(&b, "{\"ok\":false,\"errno\":") ||
      buf_reserve(&b, 32))
  {
    free(b.p);
    return dup_str("{\"ok\":false,\"error\":\"oom\"}");
  }
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%d", err);
  if (buf_puts(&b, tmp) ||
      buf_puts(&b, ",\"error\":") ||
      buf_put_json_str(&b, msg ? msg : ""))
  {
    free(b.p);
    return dup_str("{\"ok\":false,\"error\":\"oom\"}");
  }
  if (sqlstate && sqlstate[0])
  {
    if (buf_puts(&b, ",\"sqlstate\":") || buf_put_json_str(&b, sqlstate))
    {
      free(b.p);
      return dup_str("{\"ok\":false,\"error\":\"oom\"}");
    }
  }
  if (buf_puts(&b, "}"))
  {
    free(b.p);
    return dup_str("{\"ok\":false,\"error\":\"oom\"}");
  }
  return b.p;
}

static char *json_err(int err, const char *msg)
{
  return json_err_state(err, msg, NULL);
}

/*
  Appends the pending result of the just-executed statement to b,
  without enclosing braces:
    "fields":[...],"types":[[type,charsetnr,flags]...],"rows":[...]
  or
    "affected":N,"rows":[]
  Returns 0 on success, -1 on server error (read mysql_errno), -2 on OOM.
*/
static int serialize_result_into(MYSQL *m, buf_t *b)
{
  MYSQL_RES *res= mysql_store_result(m);
  if (!res)
  {
    if (mysql_field_count(m) != 0)
      return -1;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "\"affected\":%llu,\"rows\":[]",
             (unsigned long long)mysql_affected_rows(m));
    return buf_puts(b, tmp) ? -2 : 0;
  }

  unsigned int nfields= mysql_num_fields(res);
  MYSQL_FIELD *fields= mysql_fetch_fields(res);
  if (buf_puts(b, "\"fields\":["))
    goto oom;
  for (unsigned int i= 0; i < nfields; i++)
  {
    if ((i && buf_putc(b, ',')) || buf_put_json_str(b, fields[i].name))
      goto oom;
  }
  if (buf_puts(b, "],\"types\":["))
    goto oom;
  for (unsigned int i= 0; i < nfields; i++)
  {
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "%s[%u,%u,%u]", i ? "," : "",
             (unsigned int)fields[i].type, fields[i].charsetnr,
             fields[i].flags);
    if (buf_puts(b, tmp))
      goto oom;
  }
  if (buf_puts(b, "],\"rows\":["))
    goto oom;

  MYSQL_ROW row;
  int first_row= 1;
  while ((row= mysql_fetch_row(res)))
  {
    unsigned long *lengths= mysql_fetch_lengths(res);
    if (!first_row && buf_putc(b, ','))
      goto oom;
    first_row= 0;
    if (buf_putc(b, '{'))
      goto oom;
    for (unsigned int i= 0; i < nfields; i++)
    {
      if (i && buf_putc(b, ','))
        goto oom;
      if (buf_put_json_str(b, fields[i].name) || buf_putc(b, ':'))
        goto oom;
      if (!row[i])
      {
        if (buf_puts(b, "null"))
          goto oom;
      }
      else if (is_binary_field(&fields[i]))
      {
        if (buf_puts(b, "{\"$h\":") ||
            buf_put_json_hex(b, row[i], lengths ? lengths[i] : strlen(row[i])) ||
            buf_putc(b, '}'))
          goto oom;
      }
      else if (buf_put_json_str(b, row[i]))
        goto oom;
    }
    if (buf_putc(b, '}'))
      goto oom;
  }
  if (buf_puts(b, "]"))
    goto oom;
  mysql_free_result(res);
  return 0;

oom:
  mysql_free_result(res);
  return -2;
}

/*
  Drains pending results of extra statements (multi-statement SQL passed to
  l4m_query). Returns 0 when clean, 1 when a later statement failed.
*/
static int drain_extra_results(MYSQL *m)
{
  int failed= 0;
  while (mysql_more_results(m))
  {
    if (mysql_next_result(m) != 0)
      return 1;
    MYSQL_RES *extra= mysql_store_result(m);
    if (extra)
      mysql_free_result(extra);
    else if (mysql_field_count(m) != 0)
      return 1;
  }
  return failed;
}

EMSCRIPTEN_KEEPALIVE
int l4m_open(void)
{
  if (g_open)
    return 0;

  ensure_dir("/tmp");
  ensure_dir("/mariadb");
  ensure_dir("/mariadb/data");
  ensure_dir("/mariadb/plugin");

  char *argv[]= {
    (char *)"lite4mariadb",
    (char *)"--basedir=/mariadb",
    (char *)"--datadir=/mariadb/data",
    (char *)"--plugin-dir=/mariadb/plugin",
    (char *)"--lc-messages-dir=/mariadb/share",
    (char *)"--character-sets-dir=/mariadb/share/charsets",
    (char *)"--tmpdir=/tmp",
    (char *)"--skip-grant-tables",
    (char *)"--skip-networking",
    (char *)"--skip-log-bin",
    (char *)"--innodb-use-native-aio=0",
    (char *)"--innodb-buffer-pool-size=16M",
    (char *)"--loose-innodb-buffer-pool-size-max=16M",
    (char *)"--innodb-log-buffer-size=2M",
    (char *)"--max-connections=10",
    (char *)"--default-storage-engine=InnoDB",
    (char *)"--loose-innodb-read-io-threads=1",
    (char *)"--loose-innodb-write-io-threads=1",
    (char *)"--loose-innodb-purge-threads=1",
    (char *)"--table-open-cache=32",
    (char *)"--thread-cache-size=0",
    (char *)"--socket=/tmp/mysql.sock",
    NULL
  };
  int argc= (int)(sizeof(argv) / sizeof(argv[0]) - 1);

  if (mysql_server_init(argc, argv, NULL))
    return 1;

  g_mysql= mysql_init(NULL);
  if (!g_mysql)
  {
    mysql_server_end();
    return 1;
  }

  if (!mysql_real_connect(g_mysql, NULL, "root", "", NULL, 0, NULL,
                          CLIENT_MULTI_STATEMENTS))
  {
    mysql_close(g_mysql);
    g_mysql= NULL;
    mysql_server_end();
    return 1;
  }

  /* JS strings are UTF-8; keep the connection charset aligned. */
  (void)mysql_query(g_mysql, "SET NAMES utf8mb4");
  (void)mysql_query(g_mysql, "CREATE DATABASE IF NOT EXISTS test");
  (void)mysql_select_db(g_mysql, "test");
  g_open= 1;
  return 0;
}

EMSCRIPTEN_KEEPALIVE
void l4m_close(void)
{
  if (g_mysql)
  {
    mysql_close(g_mysql);
    g_mysql= NULL;
  }
  if (g_open)
  {
    mysql_server_end();
    g_open= 0;
  }
}

EMSCRIPTEN_KEEPALIVE
void l4m_free(char *p)
{
  free(p);
}

EMSCRIPTEN_KEEPALIVE
char *l4m_escape(const char *s)
{
  if (!g_open || !g_mysql || !s)
    return dup_str("");
  size_t len= strlen(s);
  char *out= (char *)malloc(len * 2 + 1);
  if (!out)
    return dup_str("");
  mysql_real_escape_string(g_mysql, out, s, (unsigned long)len);
  return out;
}

EMSCRIPTEN_KEEPALIVE
char *l4m_query(const char *sql)
{
  if (!g_open || !g_mysql)
    return json_err(1, "not open");
  if (!sql)
    return json_err(1, "null sql");

  if (mysql_query(g_mysql, sql))
    return json_err_state(mysql_errno(g_mysql), mysql_error(g_mysql),
                          mysql_sqlstate(g_mysql));

  buf_t b= {0};
  if (buf_puts(&b, "{\"ok\":true,"))
    goto oom;
  {
    int rc= serialize_result_into(g_mysql, &b);
    if (rc == -1)
    {
      free(b.p);
      return json_err_state(mysql_errno(g_mysql), mysql_error(g_mysql),
                            mysql_sqlstate(g_mysql));
    }
    if (rc == -2)
      goto oom;
  }
  if (drain_extra_results(g_mysql))
  {
    free(b.p);
    return json_err_state(mysql_errno(g_mysql), mysql_error(g_mysql),
                          mysql_sqlstate(g_mysql));
  }
  if (buf_putc(&b, '}'))
    goto oom;
  return b.p;

oom:
  free(b.p);
  return dup_str("{\"ok\":false,\"error\":\"oom\"}");
}

#ifdef __wasi__
/*
  WASIX CLI entry point: opens the embedded server, runs each argument as
  a statement and prints one JSON envelope per statement.

  wasmer run lite4mariadb.wasix.wasm \
    --mapdir /mariadb:$(pwd)/wasix-data --mapdir /tmp:/tmp \
    -- "CREATE TABLE t1 (id INT)" "INSERT INTO t1 VALUES (1)" "SELECT * FROM t1"
*/
int main(int argc, char **argv)
{
  if (l4m_open())
  {
    fprintf(stderr, "lite4mariadb: server init failed\n");
    return 1;
  }
  int rc= 0;
  for (int i= 1; i < argc; i++)
  {
    char *r= l4m_query(argv[i]);
    if (!r)
    {
      rc= 1;
      continue;
    }
    puts(r);
    if (strncmp(r, "{\"ok\":true", 10) != 0)
      rc= 1;
    l4m_free(r);
  }
  l4m_close();
  return rc;
}
#endif

EMSCRIPTEN_KEEPALIVE
char *l4m_exec_multi(const char *sql)
{
  if (!g_open || !g_mysql)
    return json_err(1, "not open");
  if (!sql)
    return json_err(1, "null sql");

  if (mysql_query(g_mysql, sql))
    return json_err_state(mysql_errno(g_mysql), mysql_error(g_mysql),
                          mysql_sqlstate(g_mysql));

  buf_t b= {0};
  if (buf_puts(&b, "{\"ok\":true,\"results\":["))
    goto oom;
  int first= 1;
  for (;;)
  {
    if ((!first && buf_putc(&b, ',')) || buf_putc(&b, '{'))
      goto oom;
    first= 0;
    int rc= serialize_result_into(g_mysql, &b);
    if (rc == -1)
    {
      free(b.p);
      return json_err_state(mysql_errno(g_mysql), mysql_error(g_mysql),
                            mysql_sqlstate(g_mysql));
    }
    if (rc == -2)
      goto oom;
    if (buf_putc(&b, '}'))
      goto oom;
    if (!mysql_more_results(g_mysql))
      break;
    if (mysql_next_result(g_mysql) != 0)
    {
      free(b.p);
      return json_err_state(mysql_errno(g_mysql), mysql_error(g_mysql),
                            mysql_sqlstate(g_mysql));
    }
  }
  if (buf_puts(&b, "]}"))
    goto oom;
  return b.p;

oom:
  free(b.p);
  return dup_str("{\"ok\":false,\"error\":\"oom\"}");
}
