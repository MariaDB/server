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

#ifdef MYSQL_CLIENT
/* opt_ssl_verify_server_cert= 0 */
#define INIT_SSL_VERIFY_CERT ,0
#else
#define INIT_SSL_VERIFY_CERT
#endif

/* Use SSL_VARS_STATIC and SSL_VARS_NON_STATIC to declare static/global variables. */
#ifdef SSL_VARS_STATIC
#define SSL_STATIC static
#else
#define SSL_STATIC
#endif


#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY) && !defined(SSL_VARS_STATIC) && !defined(SSL_VARS_NON_STATIC)
/*
 * opt_use_ssl    = 1
 * opt_ssl_ca     = 0
 * opt_ssl_capath = 0
 * opt_ssl_cert   = 0
 * opt_ssl_cipher = 0
 * opt_ssl_key    = 0
 * opt_ssl_crl    = 0
 * opt_ssl_crlpath= 0
 * opt_tls_version= 0
 */
#define INIT_SLL_OPTS , 1, 0, 0, 0, 0, 0, 0, 0, 0 INIT_SSL_VERIFY_CERT
#else
#define INIT_SLL_OPTS
#endif /* HAVE_OPENSSL */

#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
#if defined(SSL_VARS_STATIC) || defined(SSL_VARS_NON_STATIC)
SSL_STATIC my_bool opt_use_ssl = 1;
#else
SSL_STATIC my_bool opt_use_ssl;
#endif
SSL_STATIC char *opt_ssl_ca;
SSL_STATIC char *opt_ssl_capath;
SSL_STATIC char *opt_ssl_cert;
SSL_STATIC char *opt_ssl_cipher;
SSL_STATIC char *opt_ssl_key;
SSL_STATIC char *opt_ssl_crl;
SSL_STATIC char *opt_ssl_crlpath;
SSL_STATIC char *opt_tls_version;
#ifdef MYSQL_CLIENT
SSL_STATIC my_bool opt_ssl_verify_server_cert;
#endif
#endif /* HAVE_OPENSSL */
#endif /* SSLOPT_VARS_INCLUDED */

