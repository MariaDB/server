#ifndef SSLOPT_CASE_INCLUDED
#define SSLOPT_CASE_INCLUDED

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
    case OPT_SSL_KEY:
    case OPT_SSL_CERT:
    case OPT_SSL_CA:
    case OPT_SSL_CAPATH:
    case OPT_SSL_CIPHER:
    case OPT_SSL_CRL:
    case OPT_SSL_CRLPATH:
    case OPT_TLS_VERSION:
#ifdef MYSQL_CLIENT
    case OPT_SSL_FP:
    case OPT_SSL_FPLIST:
#endif
    /*
      Enable use of SSL if we are using any ssl option
      One can disable SSL later by using --skip-ssl or --ssl=0
    */
      opt_use_ssl= 1;
#if defined (HAVE_WOLFSSL)
#if defined(MYSQL_SERVER)
      /* CRL does not work with WolfSSL (server) */
      opt_ssl_crl= NULL;
#endif
#if !defined(_WIN32) || !defined(LIBMARIADB)
      /* CRL_PATH does not work with WolfSSL (server) and GnuTLS (client) */
      opt_ssl_crlpath= NULL;
#endif
#endif
      break;
#endif
#endif /* SSLOPT_CASE_INCLUDED */
