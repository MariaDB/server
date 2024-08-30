#ifndef SSLOPT_VARS_INCLUDED
#define SSLOPT_VARS_INCLUDED

/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
#ifdef SSL_VARS_NOT_STATIC
#define SSL_STATIC
#else
#define SSL_STATIC static
#endif
SSL_STATIC my_bool opt_use_ssl   = 1;
SSL_STATIC char *opt_ssl_ca      = 0;
SSL_STATIC char *opt_ssl_capath  = 0;
SSL_STATIC char *opt_ssl_cert    = 0;
SSL_STATIC char *opt_ssl_cipher  = 0;
SSL_STATIC char *opt_ssl_key     = 0;
SSL_STATIC char *opt_ssl_crl     = 0;
SSL_STATIC char *opt_ssl_crlpath = 0;
SSL_STATIC char *opt_tls_version = 0;
#ifdef MYSQL_CLIENT
SSL_STATIC char *opt_ssl_fp      = 0;
SSL_STATIC char *opt_ssl_fplist  = 0;
SSL_STATIC my_bool opt_ssl_verify_server_cert= 2;

#define SET_SSL_OPTS(M)                                                 \
  do {                                                                  \
    if (opt_use_ssl)                                                    \
    {                                                                   \
      mysql_ssl_set((M), opt_ssl_key, opt_ssl_cert, opt_ssl_ca,         \
                    opt_ssl_capath, opt_ssl_cipher);                    \
      mysql_options((M), MYSQL_OPT_SSL_CRL, opt_ssl_crl);               \
      mysql_options((M), MYSQL_OPT_SSL_CRLPATH, opt_ssl_crlpath);       \
      mysql_options((M), MARIADB_OPT_TLS_VERSION, opt_tls_version);     \
      mysql_options((M), MARIADB_OPT_TLS_PEER_FP, opt_ssl_fp);          \
      mysql_options((M), MARIADB_OPT_TLS_PEER_FP_LIST, opt_ssl_fplist); \
    }                                                                   \
    else                                                                \
      opt_ssl_verify_server_cert= 0;                                    \
    mysql_options((M),MYSQL_OPT_SSL_VERIFY_SERVER_CERT,                 \
                  &opt_ssl_verify_server_cert);                         \
  } while(0)

/*
  let's disable opt_ssl_verify_server_cert if neither CA nor FP and
  nor password were specified and the protocol is TCP.
*/
#define SET_SSL_OPTS_WITH_CHECK(M)                                      \
  do {                                                                  \
    if (opt_use_ssl && opt_ssl_verify_server_cert==2 &&                 \
        !(opt_ssl_ca && opt_ssl_ca[0]) &&                               \
        !(opt_ssl_capath && opt_ssl_capath[0]) &&                       \
        !(opt_ssl_fp && opt_ssl_fp[0]) &&                               \
        !(opt_ssl_fplist && opt_ssl_fplist[0]) &&                       \
        !(opt_password && opt_password[0]) &&                           \
        opt_protocol == MYSQL_PROTOCOL_TCP)                             \
    {                                                                   \
      fprintf(stderr, "WARNING: option --ssl-verify-server-cert is "    \
              "disabled, because of an insecure passwordless login.\n");\
      opt_ssl_verify_server_cert= 0;                                    \
    }                                                                   \
    SET_SSL_OPTS(M);                                                    \
  } while (0)

#endif
#else
#define SET_SSL_OPTS(M) do { } while(0)
#define SET_SSL_OPTS_WITH_CHECK(M) do { } while(0)
#endif
#endif /* SSLOPT_VARS_INCLUDED */
