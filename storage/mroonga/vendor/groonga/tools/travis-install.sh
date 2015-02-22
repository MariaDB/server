#!/bin/sh

set -e

case "${TRAVIS_OS_NAME}" in
  linux)
    curl --silent --location https://raw.github.com/clear-code/cutter/master/data/travis/setup.sh | sh
    sudo apt-get install -qq -y \
         autotools-dev \
         zlib1g-dev \
         libmsgpack-dev \
         libevent-dev \
         libmecab-dev \
         mecab-naist-jdic \
         cmake
    if [ "${ENABLE_JEMALLOC}" = "yes" ]; then
      sudo apt-get install -qq -y libjemalloc-dev
    fi
    ;;
  osx)
    brew install \
         msgpack \
         libevent \
         mecab \
         mecab-ipadic
    ;;
esac

if [ "${ENABLE_MRUBY}" = "yes" ]; then
  gem install pkg-config groonga-client
fi
