#!/bin/sh

LANG=C

run()
{
  "$@"
  if test $? -ne 0; then
    echo "Failed $@"
    exit 1
  fi
}

. /vagrant/tmp/env.sh

run apt-get update
run apt-get install -y lsb-release

distribution=$(lsb_release --id --short | tr 'A-Z' 'a-z')
code_name=$(lsb_release --codename --short)
case "${distribution}" in
  debian)
    component=main
    run cat <<EOF > /etc/apt/sources.list.d/groonga.list
deb http://packages.groonga.org/debian/ ${code_name} main
deb-src http://packages.groonga.org/debian/ ${code_name} main
EOF
    run apt-get update
    run apt-get install -y --allow-unauthenticated groonga-keyring
    run apt-get update
    ;;
  ubuntu)
    component=universe
    run apt-get -y install software-properties-common
    run add-apt-repository -y universe
    run add-apt-repository -y ppa:groonga/ppa
    run apt-get update
    ;;
esac

run apt-get install -V -y build-essential devscripts ${DEPENDED_PACKAGES}

run mkdir -p build
run cp /vagrant/tmp/${PACKAGE}-${VERSION}.tar.gz \
  build/${PACKAGE}_${VERSION}.orig.tar.gz
run cd build
run tar xfz ${PACKAGE}_${VERSION}.orig.tar.gz
run cd ${PACKAGE}-${VERSION}/
run cp -rp /vagrant/tmp/debian debian
# export DEB_BUILD_OPTIONS=noopt
run debuild -us -uc
run cd -

package_initial=$(echo "${PACKAGE}" | sed -e 's/\(.\).*/\1/')
pool_dir="/vagrant/repositories/${distribution}/pool/${code_name}/${component}/${package_initial}/${PACKAGE}"
run mkdir -p "${pool_dir}/"
run cp *.tar.* *.dsc *.deb "${pool_dir}/"
