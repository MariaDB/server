#! /bin/sh
#
srcdir="$1"
pcretest="$2"
cd "$3"
shift
shift
shift
. "$srcdir"/RunTest
if test "$?" != "0"; then exit 1; fi
# End
