#!/bin/bash
#
# Copyright(C) 2012-2015 Kouhei Sutou <kou@clear-code.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA

# set -x
set -e

if [ "${MROONGA_BUNDLED}" = "yes" ]; then
  cmake_args=(-DCMAKE_BUILD_TYPE=Debug -DWITH_UNIT_TESTS=FALSE)
  cmake_args=("${cmake_args[@]}" -DWITH_EMBEDDED_SERVER=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_ARCHIVE=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_BLACKHOLE=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_CASSANDRA=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_CONNECT=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_CSV=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_EXAMPLE=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_FEDERATED=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_FEDERATEDX=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_HEAP=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_MYISAMMRG=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_OQGRAPH=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_SEQUENCE=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_SPHINX=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_SPIDER=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_TEST_SQL_DISCOVERY=TRUE)
  cmake_args=("${cmake_args[@]}" -DWITHOUT_TOKUDB=TRUE)
  if [ "${MROONGA_TEST_EMBEDDED}" = "yes" ]; then
    cmake_args=("${cmake_args[@]}" -DWITH_EMBEDDED_SERVER=TRUE)
  fi
  cmake . "${cmake_args[@]}"
else
  ./autogen.sh

  if [ -d /opt/mysql/ ]; then
    PATH=$(echo /opt/mysql/server-*/bin/):$PATH
  fi
  configure_args=("--with-mysql-source=$PWD/vendor/mysql")
  if [ "${MYSQL_VERSION}" = "mysql-5.6.25" ]; then
    configure_args=("${configure_args[@]}" --enable-fast-mutexes)
  fi
  ./configure "${configure_args[@]}"
  cat "$(mysql_config --include | sed -e 's/-I//g')/my_config.h"
fi
