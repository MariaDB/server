# Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
# 
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation; version 2 of the
# License.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA

IF(WIN32)
  RETURN()
ENDIF()

CHECK_CXX_SOURCE_COMPILES(
"#define _GNU_SOURCE
#include <sys/socket.h>
int main() {
  struct ucred cred;
  getsockopt(0, SOL_SOCKET, SO_PEERCRED, &cred, 0);
}" HAVE_PEERCRED)
 
IF (HAVE_PEERCRED)
  ADD_DEFINITIONS(-DHAVE_PEERCRED)
  SET(ok 1)
ELSE()

# Hi, OpenBSD!
CHECK_CXX_SOURCE_COMPILES(
"#include <sys/types.h>
#include <sys/socket.h>
int main() {
  struct sockpeercred cred;
  getsockopt(0, SOL_SOCKET, SO_PEERCRED, &cred, 0);
  }" HAVE_SOCKPEERCRED)

IF (HAVE_SOCKPEERCRED)
  ADD_DEFINITIONS(-DHAVE_SOCKPEERCRED)
  SET(ok 1)
ELSE()

# FreeBSD, is that you?
CHECK_CXX_SOURCE_COMPILES(
"#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ucred.h>
int main() {
  struct xucred cred;
  getsockopt(0, 0, LOCAL_PEERCRED, &cred, 0);
  }" HAVE_XUCRED)

IF (HAVE_XUCRED)
  ADD_DEFINITIONS(-DHAVE_XUCRED)
  SET(ok 1)
ELSE()

# NetBSD, is that you?
CHECK_CXX_SOURCE_COMPILES(
"#include <sys/un.h>
#include <sys/socket.h>
int main() {
  struct unpcbid unp;
  socklen_t unpl = sizeof(unp);
  getsockopt(0, 0, LOCAL_PEEREID, &unp, &unpl);
  }" HAVE_UNPCBID)

IF (HAVE_UNPCBID)
  ADD_DEFINITIONS(-DHAVE_UNPCBID)
  SET(ok 1)
ELSE()

# illumos, is that you?
CHECK_CXX_SOURCE_COMPILES(
"#include <ucred.h>
int main() {
  ucred_t *cred = NULL;
  getpeerucred(0, &cred);
  }" HAVE_GETPEERUCRED)

# Depending on the flags set in the compilation environment, illumos will have
# either the POSIX.1c draft 6 or POSIX.1c final implementation of getpwuid_r()
# Check that defining _POSIX_PTHREAD_SEMANTICS provides the final standard
# version.

CHECK_CXX_SOURCE_COMPILES(
"#define _POSIX_PTHREAD_SEMANTICS
#include <pwd.h>
int main() {
  getpwuid_r(0, NULL, NULL, 0, NULL);
  }" HAVE_GETPWUID_POSIX_FINAL)

IF (HAVE_GETPEERUCRED AND HAVE_GETPWUID_POSIX_FINAL)
  ADD_DEFINITIONS(-DHAVE_GETPEERUCRED)
  ADD_DEFINITIONS(-D_POSIX_PTHREAD_SEMANTICS)
  SET(ok 1)
ELSE()

# AIX also!
CHECK_CXX_SOURCE_COMPILES(
"#include <sys/socket.h>
int main() {
  struct peercred_struct cred;
  getsockopt(0, SOL_SOCKET, SO_PEERID, &cred, 0);
  }" HAVE_PEERCRED_STRUCT)

IF (HAVE_PEERCRED_STRUCT)
  ADD_DEFINITIONS(-DHAVE_PEERCRED_STRUCT)
  SET(ok 1)
ELSE()

# Who else? Anyone?
# C'mon, show your creativity, be different! ifdef's are fun, aren't they?

ENDIF()
ENDIF()
ENDIF()
ENDIF()
ENDIF()
ENDIF()

IF(ok)
  MYSQL_ADD_PLUGIN(auth_socket auth_socket.c DEFAULT)
ENDIF()
