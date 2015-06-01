#!/bin/sh

set -e

prefix=/tmp/local

command_test_options="--n-workers=4"

set -x

case "${BUILD_TOOL}" in
  autotools)
    test/unit/run-test.sh
    test/command/run-test.sh ${command_test_options}
    if [ "${ENABLE_MRUBY}" = "yes" ]; then
      test/query_optimizer/run-test.rb
    fi
    test/command/run-test.sh ${command_test_options} --interface http
    mkdir -p ${prefix}/var/log/groonga/httpd
    test/command/run-test.sh ${command_test_options} --testee groonga-httpd
    ;;
  cmake)
    test/command/run-test.sh ${command_test_options}
    ;;
esac
