#!/bin/sh

script_base_dir=`dirname $0`

if [ $# != 2 ]; then
    echo "Usage: $0 DESTINATION DISTRIBUTIONS"
    echo " e.g.: $0 repositories/ 'fedora centos'"
    exit 1
fi

DESTINATION=$1
DISTRIBUTIONS=$2

run()
{
    "$@"
    if test $? -ne 0; then
	echo "Failed $@"
	exit 1
    fi
}

for distribution in ${DISTRIBUTIONS}; do
    for dir in ${DESTINATION}${distribution}/*/*; do
	test -d $dir &&	run createrepo $dir
    done;
done
