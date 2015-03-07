#!/bin/sh

set -e

prefix=/tmp/local

case "${BUILD_TOOL}" in
  autotools)
    test/unit/run-test.sh
    test/command/run-test.sh
    if [ "${ENABLE_MRUBY}" = "yes" ]; then
      test/query_optimizer/run-test.rb
    fi
    test/command/run-test.sh --interface http
    mkdir -p ${prefix}/var/log/groonga/httpd
    test/command/run-test.sh --testee groonga-httpd
    ;;
  cmake)
    test/command/run-test.sh
    ;;
esac
