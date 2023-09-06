#ifndef SSLOPT_LONGOPTS_INCLUDED
#define SSLOPT_LONGOPTS_INCLUDED

/*
   Copyright (c) 2000, 2010, Oracle and/or its affiliates.

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


#define EMBED_SSL_LONGOPTS_HAVE_OPENSSL_INTERNAL(...)
#define EMBED_SSL_LONGOPTS_MARIADB_CLIENT_INTERNAL(...)
#define SSL_L_PREF_EXP0_INTERNAL()
#define SSL_L_PREF_EXP1_INTERNAL(pref) pref

#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
/*
 Embed main ssl long options with specified prefix
 which can be either "variadble_name." or an empty placeholder.
 Note: internal usage. For indirect call only.
*/
#undef EMBED_SSL_LONGOPTS_HAVE_OPENSSL_INTERNAL
#define EMBED_SSL_LONGOPTS_HAVE_OPENSSL_INTERNAL(PREF_EXP, ...)                     \
  {"ssl", OPT_SSL_SSL,                                                              \
   "Enable SSL for connection (automatically enabled with other flags).",           \
   &PREF_EXP(__VA_ARGS__) opt_use_ssl, &PREF_EXP(__VA_ARGS__) opt_use_ssl,          \
   0, GET_BOOL, OPT_ARG, 1, 0, 0, 0, 0, 0},                                         \
  {"ssl-ca", OPT_SSL_CA,                                                            \
   "CA file in PEM format (check OpenSSL docs, implies --ssl).",                    \
   &PREF_EXP(__VA_ARGS__) opt_ssl_ca, &PREF_EXP(__VA_ARGS__) opt_ssl_ca,            \
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},                                     \
  {"ssl-capath", OPT_SSL_CAPATH,                                                    \
   "CA directory (check OpenSSL docs, implies --ssl).",                             \
   &PREF_EXP(__VA_ARGS__) opt_ssl_capath, &PREF_EXP(__VA_ARGS__) opt_ssl_capath,    \
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},                                     \
  {"ssl-cert", OPT_SSL_CERT, "X509 cert in PEM format (implies --ssl).",            \
   &PREF_EXP(__VA_ARGS__) opt_ssl_cert, &PREF_EXP(__VA_ARGS__) opt_ssl_cert,        \
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},                                     \
  {"ssl-cipher", OPT_SSL_CIPHER, "SSL cipher to use (implies --ssl).",              \
   &PREF_EXP(__VA_ARGS__) opt_ssl_cipher, &PREF_EXP(__VA_ARGS__) opt_ssl_cipher,    \
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},                                     \
  {"ssl-key", OPT_SSL_KEY, "X509 key in PEM format (implies --ssl).",               \
   &PREF_EXP(__VA_ARGS__) opt_ssl_key, &PREF_EXP(__VA_ARGS__) opt_ssl_key,          \
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},                                     \
  {"ssl-crl", OPT_SSL_CRL, "Certificate revocation list (implies --ssl).",          \
   &PREF_EXP(__VA_ARGS__) opt_ssl_crl, &PREF_EXP(__VA_ARGS__) opt_ssl_crl,          \
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},                                     \
  {"ssl-crlpath", OPT_SSL_CRLPATH,                                                  \
    "Certificate revocation list path (implies --ssl).",                            \
   &PREF_EXP(__VA_ARGS__) opt_ssl_crlpath, &PREF_EXP(__VA_ARGS__) opt_ssl_crlpath,  \
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},                                     \
  {"tls-version", OPT_TLS_VERSION,                                                  \
   "TLS protocol version for secure connection.",                                   \
   &PREF_EXP(__VA_ARGS__) opt_tls_version, &PREF_EXP(__VA_ARGS__) opt_tls_version,  \
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},


#ifdef MYSQL_CLIENT
/*
 Embed "opt_ssl_verify_server_cert" ssl option with specified prefix
 which can be either "variadble_name." or an empty placeholder.
 Note: internal usage. For indirect call only.
*/
#undef EMBED_SSL_LONGOPTS_MARIADB_CLIENT_INTERNAL
#define EMBED_SSL_LONGOPTS_MARIADB_CLIENT_INTERNAL(PREF_EXP, ...)        \
  {"ssl-verify-server-cert", OPT_SSL_VERIFY_SERVER_CERT,                 \
   "Verify server's \"Common Name\" in its cert against hostname used "  \
   "when connecting. This option is disabled by default.",               \
   &PREF_EXP(__VA_ARGS__) opt_ssl_verify_server_cert,                    \
   &PREF_EXP(__VA_ARGS__) opt_ssl_verify_server_cert,                    \
   0, GET_BOOL, OPT_ARG, 0, 0, 0, 0, 0, 0},

#endif  /* MYSQL_CLIENT */
#endif  /* HAVE_OPENSSL */


#if !defined(SSL_VARS_STATIC) && !defined(SSL_VARS_NON_STATIC)
/* The macro to embed ssl long options as part of the client connection struct */
#define SSL_LONGOPTS_EMBED(opts_varname)                                      \
EMBED_SSL_LONGOPTS_HAVE_OPENSSL_INTERNAL(SSL_L_PREF_EXP1_INTERNAL, opts_varname.)   \
EMBED_SSL_LONGOPTS_MARIADB_CLIENT_INTERNAL(SSL_L_PREF_EXP1_INTERNAL, opts_varname.)
#else
/* The macro to embed ssl long options as separate variables */
#define SSL_LONGOPTS_EMBED_VARS                                 \
EMBED_SSL_LONGOPTS_HAVE_OPENSSL_INTERNAL(SSL_L_PREF_EXP0_INTERNAL)    \
EMBED_SSL_LONGOPTS_MARIADB_CLIENT_INTERNAL(SSL_L_PREF_EXP0_INTERNAL)
#endif  /* SSL_VARS_STATIC */
#endif  /* SSLOPT_LONGOPTS_INCLUDED */