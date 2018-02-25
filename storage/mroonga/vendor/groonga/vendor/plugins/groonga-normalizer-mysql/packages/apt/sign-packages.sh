#!/bin/sh

script_base_dir=`dirname $0`

if [ $# != 3 ]; then
    echo "Usage: $0 GPG_UID DESITINATION CODES"
    echo " e.g.: $0 'F10399C0' repositories/ 'lenny unstable hardy karmic'"
    exit 1
fi

GPG_UID=$1
DESTINATION=$2
CODES=$3

run()
{
    "$@"
    if test $? -ne 0; then
	echo "Failed $@"
	exit 1
    fi
}

for code_name in ${CODES}; do
    case ${code_name} in
	jessie|unstable)
	    distribution=debian
	    ;;
	*)
	    distribution=ubuntu
	    ;;
    esac

    base_directory=${DESTINATION}${distribution}
    debsign -pgpg2 --re-sign -k${GPG_UID} \
	$(find ${base_directory} -name '*.dsc' -or -name '*.changes') &
    if [ "${PARALLEL}" != "yes" ]; then
	wait
    fi
done

wait
