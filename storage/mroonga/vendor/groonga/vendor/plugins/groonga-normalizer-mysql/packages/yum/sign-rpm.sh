#!/bin/sh

script_base_dir=`dirname $0`

if [ $# != 3 ]; then
    echo "Usage: $0 GPG_UID DESTINATION DISTRIBUTIONS"
    echo " e.g.: $0 'F10399C0' repositories/ 'fedora centos'"
    exit 1
fi

GPG_UID=$1
DESTINATION=$2
DISTRIBUTIONS=$3

run()
{
    "$@"
    if test $? -ne 0; then
	echo "Failed $@"
	exit 1
    fi
}

rpms=""
for distribution in ${DISTRIBUTIONS}; do
    rpms="${rpms} $(echo ${DESTINATION}${distribution}/*/*/*/*.rpm)"
done

echo "NOTE: YOU JUST ENTER! YOU DON'T NEED TO INPUT PASSWORD!"
echo "      IT'S JUST FOR rpm COMMAND RESTRICTION!"
run echo $rpms | xargs rpm \
    -D "_gpg_name ${GPG_UID}" \
    -D "_gpg_digest_algo sha1" \
    -D "__gpg /usr/bin/gpg2" \
    -D "__gpg_check_password_cmd /bin/true true" \
    -D "__gpg_sign_cmd %{__gpg} gpg --batch --no-verbose --no-armor %{?_gpg_digest_algo:--digest-algo %{_gpg_digest_algo}} --no-secmem-warning -u \"%{_gpg_name}\" -sbo %{__signature_filename} %{__plaintext_filename}" \
    --resign
