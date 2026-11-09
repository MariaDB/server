/* Copyright (c) 2010 Sergei Golubchik and Monty Program Ab
   Copyright (c) 2010, 2011, Oracle and/or its affiliates.
   Copyright (c) 2026, MariaDB plc.
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; version 2 of the
    License.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA */

/**
  @file

  auth_socket authentication plugin.

  Authentication is successful if the connection is done via a unix socket and
  the owner of the client process matches the user name that was used when
  connecting to mysqld. Connections from the server process itself (storage
  engines connecting to the local server) are rejected, where the platform
  reports the peer pid.
*/
#define _GNU_SOURCE 1 /* for struct ucred */

#include <mysql/plugin_auth.h>
#include <string.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef HAVE_PEERCRED
#define level SOL_SOCKET
#define PEER_PID(S) cred.pid

#elif defined HAVE_SOCKPEERCRED
#define level SOL_SOCKET
#define ucred sockpeercred
#define PEER_PID(S) cred.pid

#elif defined HAVE_XUCRED
#include <sys/un.h>
#include <sys/ucred.h>
#define level 0
#define SO_PEERCRED LOCAL_PEERCRED
#define uid cr_uid
#define ucred xucred
#ifdef HAVE_XUCRED_CR_PID
#define PEER_PID(S) cred.cr_pid
#elif defined HAVE_LOCAL_PEERPID
static pid_t PEER_PID(int s)
{
  pid_t p;
  socklen_t cred_len= sizeof(p);
  if (getsockopt(s, SOL_LOCAL, LOCAL_PEERPID, &p, &cred_len))
    return getpid(); /* getsockopt failed - disallow a connection */
  return p;
}
#else
#define PEER_PID(S) 0
#endif

#elif defined HAVE_UNPCBID
#include <sys/un.h>
#define level 0
#define SO_PEERCRED LOCAL_PEEREID
#define uid unp_euid
#define ucred unpcbid
#define PEER_PID(S) cred.unp_pid

#elif defined HAVE_GETPEERUCRED
#include <ucred.h>

#elif defined HAVE_PEERCRED_STRUCT
#define level SOL_SOCKET
#define SO_PEERCRED SO_PEERID
#define uid euid
#define ucred peercred_struct
#define PEER_PID(S) cred.pid

#else
#error impossible
#endif

/**
  perform the unix socket based authentication

  This authentication callback performs a unix socket based authentication -
  it gets the uid of the client process and considers the user authenticated
  if it uses username of this uid. That is - if the user is already
  authenticated to the OS (if she is logged in) - she can use MySQL as herself
*/

static int socket_auth(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info)
{
  unsigned char *pkt;
  MYSQL_PLUGIN_VIO_INFO vio_info;
#ifdef HAVE_GETPEERUCRED
  ucred_t *cred = NULL;
#else
  struct ucred cred;
  socklen_t cred_len= sizeof(cred);
#endif
  struct passwd pwd_buf, *pwd;
  char buf[1024];
  uid_t u;
  pid_t p;

  /* no user name yet ? read the client handshake packet with the user name */
  if (info->user_name == 0)
  {
    if (vio->read_packet(vio, &pkt) < 0)
      return CR_ERROR;
  }

  info->password_used= PASSWORD_USED_NO_MENTION;

  vio->info(vio, &vio_info);
  if (vio_info.protocol != MYSQL_VIO_SOCKET)
    return CR_ERROR;

  /* get the UID and the PID of the client process */
#ifdef HAVE_GETPEERUCRED
  if (getpeerucred(vio_info.socket, &cred) != 0)
    return CR_ERROR;
  u = ucred_geteuid(cred);
  p = ucred_getpid(cred);
  ucred_free(cred);
#else
  if (getsockopt(vio_info.socket, level, SO_PEERCRED, &cred, &cred_len))
    return CR_ERROR;

  if (cred_len != sizeof(cred))
    return CR_ERROR;

  u = cred.uid;
  p = PEER_PID(vio_info.socket);
#endif

  /*
    the server process itself must not authenticate as the OS user
    it runs under
  */
  if (p == getpid())
    return CR_ERROR;

  /* and find the username for this uid */
  getpwuid_r(u, &pwd_buf, buf, sizeof(buf), &pwd);
  if (pwd == NULL)
    return CR_ERROR;

  /* now it's simple as that */
  return strcmp(pwd->pw_name, info->user_name) ? CR_ERROR : CR_OK;
}

static struct st_mysql_auth socket_auth_handler=
{
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  0,
  socket_auth,
  NULL, NULL /* no PASSWORD() */
};

maria_declare_plugin(auth_socket)
{
  MYSQL_AUTHENTICATION_PLUGIN,
  &socket_auth_handler,
  "unix_socket",
  "Sergei Golubchik",
  "Unix Socket based authentication",
  PLUGIN_LICENSE_GPL,
  NULL,
  NULL,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;

