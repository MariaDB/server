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

code_name=$(lsb_release --codename --short)
case "${MYSQL_VARIANT}" in
  mariadb-*)
    case "${code_name}" in
      stretch)
        mysql_server_package=mariadb-server-10.1
        MYSQL_VARIANT=mariadb-10.1
        ;;
      *)
        mysql_server_package=mariadb-server-${MYSQL_VARIANT##mariadb-}
        ;;
    esac
    DEPENDED_PACKAGES="${DEPENDED_PACKAGES} libmariadb-client-lgpl-dev"
    DEPENDED_PACKAGES="${DEPENDED_PACKAGES} libmariadbd-dev"
    ;;
  *)
    mysql_server_package=mysql-server-${MYSQL_VARIANT}
    DEPENDED_PACKAGES="${DEPENDED_PACKAGES} libmysqlclient-dev"
    DEPENDED_PACKAGES="${DEPENDED_PACKAGES} libmysqld-dev"
    ;;
esac

grep '^deb ' /etc/apt/sources.list | \
    sed -e 's/^deb /deb-src /' > /etc/apt/sources.list.d/base-source.list

run sudo sed -i'' -e 's/httpredir/ftp.jp/g' /etc/apt/sources.list

run apt-get update
run apt-get install -y lsb-release

distribution=$(lsb_release --id --short | tr 'A-Z' 'a-z')
case "${distribution}" in
  debian)
    component=main
    run cat <<EOF > /etc/apt/sources.list.d/groonga.list
deb http://packages.groonga.org/debian/ ${code_name} main
deb-src http://packages.groonga.org/debian/ ${code_name} main
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
run cd build
run tar xfz /vagrant/tmp/${PACKAGE}-${VERSION}.tar.gz
run mv ${PACKAGE}-${VERSION} ${PACKAGE}-${MYSQL_VARIANT}-${VERSION}
run tar cfz ${PACKAGE}-${MYSQL_VARIANT}_${VERSION}.orig.tar.gz \
  ${PACKAGE}-${MYSQL_VARIANT}-${VERSION}
run cd ${PACKAGE}-${MYSQL_VARIANT}-${VERSION}/
run cp -rp /vagrant/tmp/debian debian
# export DEB_BUILD_OPTIONS=noopt
MYSQL_PACKAGE_INFO=$(apt-cache show ${mysql_server_package} |
                        grep Version |
                        sort |
                        tail -1)
MYSQL_PACKAGE_VERSION=${MYSQL_PACKAGE_INFO##Version: }
sed -i'' \
    -e "s/MYSQL_VERSION/$MYSQL_PACKAGE_VERSION/g" \
    -e "s/MARIADB_VERSION/$MYSQL_PACKAGE_VERSION/g" \
    debian/control
run debuild -us -uc
run cd -

package_initial=$(echo "${PACKAGE}" | sed -e 's/\(.\).*/\1/')
pool_dir="/vagrant/repositories/${distribution}/pool/${code_name}/${component}/${package_initial}/${PACKAGE}"
run mkdir -p "${pool_dir}/"
run cp *.tar.* *.diff.gz *.dsc *.deb "${pool_dir}/"
