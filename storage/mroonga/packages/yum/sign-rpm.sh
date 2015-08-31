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

unsigned_rpms()
{
    while read rpm; do
	rpm --checksig "$rpm" | grep -v 'gpg OK' | grep -v 'MISSING KEYS' | cut -d":" -f1
    done
}

if ! gpg --list-keys "${GPG_UID}" > /dev/null 2>&1; then
    run gpg --keyserver keyserver.ubuntu.com --recv-key "${GPG_UID}"
fi
run mkdir -p tmp
run gpg --armor --export "${GPG_UID}" > tmp/sign-key
run rpm --import tmp/sign-key
run rm -rf tmp/sign-key

rpms=""
for distribution in ${DISTRIBUTIONS}; do
    rpms="${rpms} $(find ${DESTINATION}${distribution} -name '*.rpm' | unsigned_rpms)"
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
