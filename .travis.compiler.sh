#!/bin/sh
set -v -x
if [[ "${TRAVIS_OS_NAME}" == 'linux' ]]; then
  if [[ "${CXX}" == 'clang++' ]]; then
    CMAKE_OPT="-DWITHOUT_TOKUDB_STORAGE_ENGINE=ON -DWITHOUT_MROONGA_STORAGE_ENGINE=ON"
    #CMAKE_OPT="${CMAKE_OPT} -DWITH_ASAN=ON"
    if which ccache ; then
      CMAKE_OPT="${CMAKE_OPT} -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
    fi
    case ${GCC_VERSION} in
      5) CXX=clang++-4.0 ;;
      6) CXX=clang++-5.0 ;;
    esac
    export CXX CC=${CXX/++/}
  elif [[ "${CXX}" == 'g++' ]]; then
    CMAKE_OPT=""
    export CXX=g++-${GCC_VERSION}
    export CC=gcc-${GCC_VERSION}
  fi
  if [[ ${GCC_VERSION} == 6 ]]; then
         wget http://mirrors.kernel.org/ubuntu/pool/universe/p/percona-xtradb-cluster-galera-2.x/percona-xtradb-cluster-galera-2.x_165-0ubuntu1_amd64.deb ;
         ar vx percona-xtradb-cluster-galera-2.x_165-0ubuntu1_amd64.deb
         tar -xJvf data.tar.xz
         export WSREP_PROVIDER=$PWD/usr/lib/libgalera_smm.so
         MYSQL_TEST_SUITES="${MYSQL_TEST_SUITES},wsrep"
  #elif [[ ${GCC_VERSION} != 5 ]]; then
    #CMAKE_OPT="${CMAKE_OPT} -DWITH_ASAN=ON"
  fi
else
  # osx_image based tests
  CMAKE_OPT="-DOPENSSL_ROOT_DIR=/usr/local/opt/openssl"
  #CMAKE_OPT="${CMAKE_OPT} -DWITH_ASAN=ON"
  if which ccache ; then
    CMAKE_OPT="${CMAKE_OPT} -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
  fi
  CMAKE_OPT="${CMAKE_OPT} -DWITHOUT_MROONGA_STORAGE_ENGINE=ON"
  if [[ "${TYPE}" == "Debug" ]]; then
    CMAKE_OPT="${CMAKE_OPT} -DWITHOUT_TOKUDB_STORAGE_ENGINE=ON"
  fi
fi

set +v +x
