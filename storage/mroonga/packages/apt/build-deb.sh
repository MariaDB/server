#!/bin/sh

LANG=C

mysql_server_package=mysql-server

run()
{
  "$@"
  if test $? -ne 0; then
    echo "Failed $@"
    exit 1
  fi
}

. /vagrant/tmp/env.sh

grep '^deb ' /etc/apt/sources.list | \
    sed -e 's/^deb /deb-src /' > /etc/apt/sources.list.d/base-source.list

run apt-get update
run apt-get install -y lsb-release

distribution=$(lsb_release --id --short | tr 'A-Z' 'a-z')
code_name=$(lsb_release --codename --short)
case "${distribution}" in
  debian)
    component=main
    run cat <<EOF > /etc/apt/sources.list.d/groonga.list
deb http://packages.groonga.org/debian/ wheezy main
deb-src http://packages.groonga.org/debian/ wheezy main
EOF
    if ! grep --quiet security /etc/apt/sources.list; then
      run cat <<EOF > /etc/apt/sources.list.d/security.list
deb http://security.debian.org/ ${code_name}/updates main
deb-src http://security.debian.org/ ${code_name}/updates main
EOF
    fi
    run apt-get update
    run apt-get install -y --allow-unauthenticated groonga-keyring
    run apt-get update
    ;;
  ubuntu)
    component=universe
    run cat <<EOF > /etc/apt/sources.list.d/security.list
deb http://security.ubuntu.com/ubuntu ${code_name}-security main restricted
deb-src http://security.ubuntu.com/ubuntu ${code_name}-security main restricted
EOF
    run sed -e 's/main/universe/' /etc/apt/sources.list > \
      /etc/apt/sources.list.d/universe.list
    run apt-get -y install software-properties-common
    run add-apt-repository -y universe
    run add-apt-repository -y ppa:groonga/ppa
    run apt-get update
    ;;
esac

run apt-get install -V -y build-essential devscripts ${DEPENDED_PACKAGES}
run apt-get build-dep -y ${mysql_server_package}

run mkdir -p build
run cp /vagrant/tmp/${PACKAGE}-${VERSION}.tar.gz \
  build/${PACKAGE}_${VERSION}.orig.tar.gz
run cd build
run tar xfz ${PACKAGE}_${VERSION}.orig.tar.gz
run cd ${PACKAGE}-${VERSION}/
run cp -rp /vagrant/tmp/debian debian
# export DEB_BUILD_OPTIONS=noopt
MYSQL_PACKAGE_INFO=$(apt-cache show mysql-server | grep Version | sort | tail -1)
MYSQL_PACKAGE_VERSION=${MYSQL_PACKAGE_INFO##Version: }
sed -i "s/MYSQL_VERSION/$MYSQL_PACKAGE_VERSION/" debian/control
run debuild -us -uc
run cd -

package_initial=$(echo "${PACKAGE}" | sed -e 's/\(.\).*/\1/')
pool_dir="/vagrant/repositories/${distribution}/pool/${code_name}/${component}/${package_initial}/${PACKAGE}"
run mkdir -p "${pool_dir}/"
run cp *.tar.gz *.diff.gz *.dsc *.deb "${pool_dir}/"
