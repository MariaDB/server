#!/bin/sh
#
# Copyright(C) 2010  Tetsuro IKEDA
# Copyright(C) 2010-2017  Kouhei Sutou <kou@clear-code.com>
# Copyright(C) 2011  Kazuhiko
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

export BASE_DIR="$(cd $(dirname $0); pwd)"
top_dir="$BASE_DIR/.."
mroonga_test_dir="${top_dir}/mysql-test/mroonga"

n_processors=1
case `uname` in
  Linux)
    n_processors="$(grep '^processor' /proc/cpuinfo | wc -l)"
    ;;
  Darwin)
    n_processors="$(/usr/sbin/sysctl -n hw.ncpu)"
    ;;
  *)
    :
    ;;
esac

if [ "$NO_MAKE" != "yes" ]; then
  MAKE_ARGS=
  if [ -n "$n_processors" ]; then
    MAKE_ARGS="-j${n_processors}"
  fi
  make $MAKE_ARGS -C $top_dir > /dev/null || exit 1
fi

. "${top_dir}/config.sh"

bundled_groonga_normalizer_mysql_dir="${top_dir}/vendor/groonga/vendor/plugins/groonga-normalizer-mysql"
if [ -d "${bundled_groonga_normalizer_mysql_dir}" ]; then
  GRN_PLUGINS_DIR="${bundled_groonga_normalizer_mysql_dir}"
  export GRN_PLUGINS_DIR
fi

maria_storage_dir="${MYSQL_SOURCE_DIR}/storage/maria"
if [ -d "${maria_storage_dir}" ]; then
  mariadb="yes"
else
  mariadb="no"
fi
percona_udf_dir="${MYSQL_SOURCE_DIR}/plugin/percona-udf"
if [ -d "${percona_udf_dir}" ]; then
  percona="yes"
else
  percona="no"
fi

source_mysql_test_dir="${MYSQL_SOURCE_DIR}/mysql-test"
build_mysql_test_dir="${MYSQL_BUILD_DIR}/mysql-test"
source_test_suites_dir="${source_mysql_test_dir}/suite"
source_test_include_dir="${source_mysql_test_dir}/include"
build_test_suites_dir="${build_mysql_test_dir}/suite"
build_test_include_dir="${build_mysql_test_dir}/include"
case "${MYSQL_VERSION}" in
  5.1.*)
    plugins_dir="${MYSQL_BUILD_DIR}/lib/mysql/plugin"
    if [ ! -d "${build_test_suites_dir}" ]; then
      mkdir -p "${build_test_suites_dir}"
    fi
    ;;
  *)
    if [ ! -d "${build_test_suites_dir}" ]; then
      ln -s "${source_test_suites_dir}" "${build_test_suites_dir}"
    fi
    if [ "${mariadb}" = "yes" ]; then
      if [ "${MRN_BUNDLED}" != "TRUE" ]; then
	mariadb_mroonga_plugin_dir="${MYSQL_BUILD_DIR}/plugin/mroonga"
	if [ ! -e "${mariadb_mroonga_plugin_dir}" ]; then
	  ln -s "${top_dir}" "${mariadb_mroonga_plugin_dir}"
	fi
      fi
      plugins_dir=
    elif [ "${percona}" = "yes" ]; then
      plugins_dir="${MYSQL_SOURCE_DIR}/lib/mysql/plugin"
    else
      plugins_dir="${MYSQL_SOURCE_DIR}/lib/plugin"
    fi
    ;;
esac

same_link_p()
{
  src=$1
  dest=$2
  if [ -L "$dest" -a "$(readlink "$dest")" = "$src" ]; then
    return 0
  else
    return 1
  fi
}

mroonga_mysql_test_suite_dir="${build_test_suites_dir}/mroonga"
if ! same_link_p "${mroonga_test_dir}" "${mroonga_mysql_test_suite_dir}"; then
  rm -rf "${mroonga_mysql_test_suite_dir}"
  ln -s "${mroonga_test_dir}" "${mroonga_mysql_test_suite_dir}"
fi

innodb_test_suite_dir="${build_test_suites_dir}/innodb"
mroonga_wrapper_innodb_test_suite_name="mroonga_wrapper_innodb"
mroonga_wrapper_innodb_test_suite_dir="${build_test_suites_dir}/${mroonga_wrapper_innodb_test_suite_name}"
mroonga_wrapper_innodb_include_dir="${mroonga_wrapper_innodb_test_suite_dir}/include/"
if [ "$0" -nt "$(dirname "${mroonga_wrapper_innodb_test_suite_dir}")" ]; then
  rm -rf "${mroonga_wrapper_innodb_test_suite_dir}"
fi
if [ ! -d "${mroonga_wrapper_innodb_test_suite_dir}" ]; then
  cp -rp "${innodb_test_suite_dir}" "${mroonga_wrapper_innodb_test_suite_dir}"
  mkdir -p "${mroonga_wrapper_innodb_include_dir}"
  cp -rp "${source_test_include_dir}"/innodb[-_]*.inc \
     "${mroonga_wrapper_innodb_include_dir}"
  ruby -i'' \
       -pe "\$_.gsub!(/\\bengine\\s*=\\s*innodb\\b([^;\\n]*)/i,
                       \"ENGINE=mroonga\\\1 COMMENT='ENGINE \\\"InnoDB\\\"'\")
             \$_.gsub!(/\\b(storage_engine\\s*=\\s*)innodb\\b([^;\\n]*)/i,
                       \"\\\1mroonga\")
             \$_.gsub!(/^(--\\s*source\\s+)(include\\/innodb)/i,
                       \"\\\1suite/mroonga_wrapper_innodb/\\\2\")
            " \
       ${mroonga_wrapper_innodb_test_suite_dir}/r/*.result \
       ${mroonga_wrapper_innodb_test_suite_dir}/t/*.test \
       ${mroonga_wrapper_innodb_test_suite_dir}/include/*.inc
  sed -i'' \
      -e '1 i --source ../mroonga/include/mroonga/have_mroonga.inc' \
      ${mroonga_wrapper_innodb_test_suite_dir}/t/*.test
fi

all_test_suite_names=""
suite_dir="${mroonga_test_dir}/.."
cd "${suite_dir}"
suite_dir="$(pwd)"
for test_suite_name in \
  $(find mroonga -type d -name 'include' '!' -prune -o \
         -type d '!' -name 'mroonga' \
         '!' -name 'include' \
         '!' -name '[tr]'); do
  if [ -n "${all_test_suite_names}" ]; then
    all_test_suite_names="${all_test_suite_names},"
  fi
  all_test_suite_names="${all_test_suite_names}${test_suite_name}"
done
cd -

if [ -n "${plugins_dir}" ]; then
  if [ -d "${top_dir}/.libs" ]; then
    make -C ${top_dir} \
	 install-pluginLTLIBRARIES \
	 plugindir=${plugins_dir} > /dev/null || \
      exit 1
  else
    mkdir -p "${plugins_dir}"
    cp "${top_dir}/ha_mroonga.so" "${plugins_dir}" || exit 1
  fi
fi

mysql_test_run_options=""
test_suite_names=""
test_names=""
while [ $# -gt 0 ]; do
  arg="$1"
  shift
  case "$arg" in
    --manual-gdb|--gdb|--client-gdb|--boot-gdb|--debug|--valgrind)
      n_processors=1
      mysql_test_run_options="${mysql_test_run_options} ${arg}"
      ;;
    --*)
      mysql_test_run_options="${mysql_test_run_options} ${arg}"
      ;;
    *)
      case "$arg" in
	*/t/*.test)
	  test_suite_name=$(echo "$arg" | sed -e 's,/t/.*\.test,,g')
	  test_suite_name=$(cd "$test_suite_name" && pwd)
	  test_name=$(echo "$arg" | sed -e 's,.*/t/\(.*\)\.test,\1,g')
	  ;;
	*)
	  if [ -d "$arg" ]; then
	    test_suite_name=$(cd "$arg" && pwd)
	  else
	    test_suite_name="$arg"
	  fi
	  test_name=""
	  ;;
      esac

      if [ -n "${test_name}" ]; then
	if [ -n "${test_names}" ]; then
	  test_names="${test_names}|"
	fi
	test_names="${test_names}${test_name}"
      fi

      test_suite_name=$(echo "$test_suite_name" | sed -e "s,^${suite_dir}/,,")
      if echo "${test_suite_names}" | grep --quiet "${test_suite_name}"; then
	continue
      fi
      if [ -n "${test_suite_names}" ]; then
	test_suite_names="${test_suite_names},"
      fi
      test_suite_names="${test_suite_names}${test_suite_name}"
      ;;
  esac
done

if [ -z "$test_suite_names" ]; then
  test_suite_names="${all_test_suite_names}"
fi

mysql_test_run_args=""
if [ "${percona}" != "yes" ]; then
  mysql_test_run_args="${mysql_test_run_args} --mem"
fi
mysql_test_run_args="${mysql_test_run_args} --parallel=${n_processors}"
mysql_test_run_args="${mysql_test_run_args} --retry=1"
mysql_test_run_args="${mysql_test_run_args} --suite=${test_suite_names}"
mysql_test_run_args="${mysql_test_run_args} --force"
mysql_test_run_args="${mysql_test_run_args} --mysqld=--loose-plugin-load-add=ha_mroonga.so"
mysql_test_run_args="${mysql_test_run_args} --mysqld=--loose-plugin-mroonga=ON"
if [ -n "$test_names" ]; then
  mysql_test_run_args="${mysql_test_run_args} --do-test=${test_names}"
fi

(cd "$build_mysql_test_dir" && \
    perl -I . ./mysql-test-run.pl \
      ${mysql_test_run_args} \
      ${mysql_test_run_options})
