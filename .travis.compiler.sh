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
  fi
  if [[ "${CXX}" == 'clang++' ]]; then
    if [[ "${CC_VERSION}" == '6' ]]; then
      export CXX=${CXX}-${CC_VERSION}.0
    else
      export CXX=${CXX}-${CC_VERSION}
    fi
    export CC=${CXX/++/}
    # excess warnings about unused include path
    export CFLAGS='-Wno-unused-command-line-argument'
    export CXXFLAGS='-Wno-unused-command-line-argument'
  elif [[ "${CXX}" == 'g++' ]]; then
    export CXX=g++-${CC_VERSION}
    export CC=gcc-${CC_VERSION}
  fi
  if [[ ${CC_VERSION} == 7 ]]; then
    export WSREP_PROVIDER=/usr/lib/galera/libgalera_smm.so
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
