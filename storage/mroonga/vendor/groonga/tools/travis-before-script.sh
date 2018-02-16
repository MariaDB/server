#!/bin/sh

set -e
set -u

: ${ENABLE_MRUBY:=no}

git submodule update --init --depth 1

prefix=/tmp/local

case "${BUILD_TOOL}" in
  autotools)
    ./autogen.sh

    configure_args=""
    if [ "${TRAVIS_OS_NAME}" = "osx" ]; then
      pkg_config_path="$(brew --prefix openssl)/lib/pkgconfig"
      configure_args="${configure_args} PKG_CONFIG_PATH=${pkg_config_path}"
    fi
    #if [ "$CC" = "clang" ]; then
      configure_args="${configure_args} --enable-debug"
    #fi
    if [ "${ENABLE_MRUBY}" = "yes" ]; then
      configure_args="${configure_args} --with-ruby --enable-mruby"
    fi

    ./configure --prefix=${prefix} --with-ruby ${configure_args}
    ;;
  cmake)
    cmake_args=""
    cmake_args="${cmake_args} -DGRN_WITH_DEBUG=yes"
    if [ "${ENABLE_MRUBY}" = "yes" ]; then
      cmake_args="${cmake_args} -DGRN_WITH_MRUBY=yes"
    fi

    cmake . ${cmake_args}
    ;;
esac

case "$(uname)" in
  Linux)
    n_processors="$(grep '^processor' /proc/cpuinfo | wc -l)"
    ;;
  Darwin)
    n_processors="$(/usr/sbin/sysctl -n hw.ncpu)"
    ;;
  *)
    n_processors="1"
    ;;
esac

make -j${n_processors} > /dev/null
