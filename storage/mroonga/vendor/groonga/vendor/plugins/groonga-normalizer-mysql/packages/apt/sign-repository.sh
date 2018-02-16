#!/bin/sh

script_base_dir=`dirname $0`

if [ $# != 3 ]; then
    echo "Usage: $0 GPG_UID DESTINATION CODES"
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

    release=${DESTINATION}${distribution}/dists/${code_name}/Release
    rm -f ${release}.gpg
    gpg2 --sign --detach-sign --armor \
	--local-user ${GPG_UID} \
	--output ${release}.gpg \
	${release} &

    if [ "${PARALLEL}" != "yes" ]; then
	wait
    fi
done

wait
