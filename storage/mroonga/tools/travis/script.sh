#!/bin/bash
#
# Copyright(C) 2012-2016 Kouhei Sutou <kou@clear-code.com>
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
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

# set -x
set -e

base_dir="$(cd $(dirname $0); pwd)"
top_dir="${base_dir}/../.."

bundled_mroonga_dir="${top_dir}/storage/mroonga"
if [ -f "${bundled_mroonga_dir}/config.sh" ]; then
  mroonga_dir="${bundled_mroonga_dir}"
  . "${bundled_mroonga_dir}/config.sh"
else
  mroonga_dir="${top_dir}"
  . "${top_dir}/config.sh"
fi

n_processors="$(grep '^processor' /proc/cpuinfo | wc -l)"
if [ "${MROONGA_BUNDLED}" = "yes" ]; then
  max_n_processors=2
else
  max_n_processors=4
fi
if (( $n_processors > $max_n_processors )); then
  n_processors=$max_n_processors
fi

build()
{
  make -j${n_processors} > /dev/null
}

run_unit_test()
{
  if [ "${MROONGA_BUNDLED}" != "yes" ]; then
    NO_MAKE=yes ${mroonga_dir}/test/run-unit-test.sh
  fi
}

prepare_mysql_test_dir()
{
  mysql_test_dir=/usr/mysql-test
  if [ -d /usr/lib/mysql-testsuite/ ]; then
    sudo cp -a /usr/lib/mysql-testsuite/ ${mysql_test_dir}/
  elif [ -d /usr/lib/mysql-test/ ]; then
    sudo cp -a /usr/lib/mysql-test/ ${mysql_test_dir}/
  elif [ -d /usr/share/mysql/mysql-test/ ]; then
    sudo cp -a /usr/share/mysql/mysql-test/ ${mysql_test_dir}/
  elif [ -d /usr/share/mysql-test/ ]; then
    sudo cp -a /usr/share/mysql-test/ ${mysql_test_dir}/
  elif [ -d /opt/mysql/ ]; then
    mysql_test_dir=$(echo /opt/mysql/server-*/mysql-test)
  else
    sudo cp -a ${MYSQL_SOURCE_DIR}/mysql-test/ ${mysql_test_dir}/
  fi
  sudo chown -R $(id -u):$(id -g) ${mysql_test_dir}/

  cp -a ${mroonga_dir}/mysql-test/mroonga/ ${mysql_test_dir}/suite/
}

collect_test_suite_names()
{
  cd ${mysql_test_dir}/suite/
  test_suite_names=""
  for test_suite_name in $(find mroonga -type d '!' -name '[tr]'); do
    if [ -n "${test_suite_names}" ]; then
      test_suite_names="${test_suite_names},"
    fi
    test_suite_names="${test_suite_names}${test_suite_name}"
  done
  cd -
}

prepare_sql_test()
{
  sudo make install > /dev/null
  prepare_mysql_test_dir
  collect_test_suite_names
}

run_sql_test()
{
  test_args=()
  if [ "${MROONGA_TEST_EMBEDDED}" = "yes" ]; then
    test_args=("${test_args[@]}" "--embedded-server")
  fi
  if [ "${MROONGA_TEST_PS_PROTOCOL}" = "yes" ]; then
    test_args=("${test_args[@]}" "--ps-protocol")
  fi

  if [ "${MROONGA_BUNDLED}" = "yes" ]; then
    # Plugins aren't supported.
    cd ${mroonga_dir}/mysql-test/mroonga/storage
    rm -rf alter_table/add_index/token_filters/
    rm -rf alter_table/t/change_token_filter.test
    rm -rf create/table/token_filters/
    rm -rf fulltext/token_filters/
    cd -

    ${mroonga_dir}/test/run-sql-test.sh \
                  "${test_args[@]}" \
                  --parallel="${n_processors}" \
                  --retry=3
  else
    prepare_sql_test

    cd ${mysql_test_dir}/
    perl ./mysql-test-run.pl \
      "${test_args[@]}" \
      --no-check-testcases \
      --parallel="${n_processors}" \
      --retry=3 \
      --suite="${test_suite_names}" \
      --force
  fi
}

build
# run_unit_test
run_sql_test
