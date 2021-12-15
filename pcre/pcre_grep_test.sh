#! /bin/sh
#
srcdir="$1"
pcregrep="$2"
pcretest="$3"
cd "$4"
shift
shift
shift
shift
. "$srcdir"/RunGrepTest
if test "$?" != "0"; then exit 1; fi
# End
