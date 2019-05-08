#!/bin/sh
set -v -x

# Exclude modules from build not directly affecting the current
# test suites found in $MYSQL_TEST_SUITES, to conserve job time
# as well as disk usage

function exclude_modules() {
# excludes for all
CMAKE_OPT="${CMAKE_OPT} -DPLUGIN_TOKUDB=NO -DPLUGIN_MROONGA=NO -DPLUGIN_SPIDER=NO -DPLUGIN_OQGRAPH=NO -DPLUGIN_PERFSCHEMA=NO -DPLUGIN_SPHINX=NO"
# exclude storage engines not being tested in current job
if [[ ! "${MYSQL_TEST_SUITES}" =~ "archive" ]]; then
  CMAKE_OPT="${CMAKE_OPT} -DPLUGIN_ARCHIVE=NO"
fi
if [[ ! "${MYSQL_TEST_SUITES}" =~ "rocksdb" ]]; then
  CMAKE_OPT="${CMAKE_OPT} -DPLUGIN_ROCKSDB=NO"
fi
}

if [[ "${TRAVIS_OS_NAME}" == 'linux' ]]; then
  TEST_CASE_TIMEOUT=2
  exclude_modules;
  if which ccache ; then
    CMAKE_OPT="${CMAKE_OPT} -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
    ccache --max-size=2200M
  fi
  if [[ "${CXX}" == 'clang++' ]]; then
    export CXX CC=${CXX/++/}
  elif [[ "${CXX}" == 'g++' ]]; then
    export CXX=g++-${CC_VERSION}
    export CC=gcc-${CC_VERSION}
  fi
  if [[ ${CC_VERSION} == 6 ]]; then
    wget http://mirrors.kernel.org/ubuntu/pool/universe/p/percona-xtradb-cluster-galera-2.x/percona-xtradb-cluster-galera-2.x_165-0ubuntu1_amd64.deb ;
    ar vx percona-xtradb-cluster-galera-2.x_165-0ubuntu1_amd64.deb
    tar -xJvf data.tar.xz
    export WSREP_PROVIDER=$PWD/usr/lib/libgalera_smm.so
    MYSQL_TEST_SUITES="${MYSQL_TEST_SUITES},wsrep"
  fi
fi

if [[ "${TRAVIS_OS_NAME}" == 'osx' ]]; then
  TEST_CASE_TIMEOUT=20
  exclude_modules;
  CMAKE_OPT="${CMAKE_OPT} -DOPENSSL_ROOT_DIR=/usr/local/opt/openssl"
  if which ccache ; then
    CMAKE_OPT="${CMAKE_OPT} -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
  fi
fi

set +v +x
