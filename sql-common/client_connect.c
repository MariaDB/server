#include <client_connect.h>
#include <my_sys.h>

MYSQL * STDCALL
do_client_connect(MYSQL *mysql, const CLNT_CONNECT_OPTIONS *opts, ulong flags)
{
  char* default_charset;

  if (opts->secure_auth)
    mysql_options(mysql, MYSQL_SECURE_AUTH, (char*) &opts->secure_auth);

  if (opts->connect_timeout)
    mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT,
                  (char*)&opts->connect_timeout);

  if (opts->read_timeout)
    mysql_options(mysql, MYSQL_OPT_READ_TIMEOUT,
                  (char*)&opts->read_timeout);

  if (opts->write_timeout)
    mysql_options(mysql, MYSQL_OPT_WRITE_TIMEOUT,
                  (char*)&opts->write_timeout);

  if (opts->compress)
    mysql_options(mysql,MYSQL_OPT_COMPRESS, NULL);

  if(opts->default_charset)
  {
    default_charset = opts->default_charset;
    if (!strcmp(default_charset,MYSQL_AUTODETECT_CHARSET_NAME))
      default_charset= (char*)my_default_csname();

    mysql_options(mysql, MYSQL_SET_CHARSET_NAME, default_charset);
  }

  if (opts->charsets_dir)
    mysql_options(mysql, MYSQL_SET_CHARSET_DIR, opts->charsets_dir);

#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
  if (opts->opt_use_ssl && opts->protocol <= MYSQL_PROTOCOL_SOCKET)
  {
    mysql_ssl_set(mysql, opts->opt_ssl_key, opts->opt_ssl_cert, opts->opt_ssl_ca,
                  opts->opt_ssl_capath, opts->opt_ssl_cipher);
    mysql_options(mysql, MYSQL_OPT_SSL_CRL, opts->opt_ssl_crl);
    mysql_options(mysql, MYSQL_OPT_SSL_CRLPATH, opts->opt_ssl_crlpath);
    mysql_options(mysql, MARIADB_OPT_TLS_VERSION, opts->opt_tls_version);
  }
#ifdef MYSQL_CLIENT
  mysql_options(mysql,MYSQL_OPT_SSL_VERIFY_SERVER_CERT,
                (char*)&opts->opt_ssl_verify_server_cert);
#endif  /* MYSQL_CLIENT */
#endif  /* HAVE_OPENSSL */

  if (opts->protocol)
    mysql_options(mysql,MYSQL_OPT_PROTOCOL, (char*)&opts->protocol);

  if (opts->plugin_dir && *opts->plugin_dir)
    mysql_options(mysql, MYSQL_PLUGIN_DIR, opts->plugin_dir);

  if (opts->default_auth && *opts->default_auth)
    mysql_options(mysql, MYSQL_DEFAULT_AUTH, opts->default_auth);

  if (opts->bind_address && *opts->bind_address)
    mysql_options(mysql, MYSQL_OPT_BIND, opts->bind_address);

  mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_RESET, 0);

  if (opts->program_name && *opts->program_name)
    mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD,
                   "program_name", opts->program_name);

  return mysql_real_connect(mysql, opts->host, opts->user,
                            opts->password, opts->database,
                            opts->port, opts->socket, flags);
}

