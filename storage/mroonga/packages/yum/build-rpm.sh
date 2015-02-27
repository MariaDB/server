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

rpmbuild_options=

. /vagrant/env.sh

distribution=$(cut -d " " -f 1 /etc/redhat-release | tr "A-Z" "a-z")
if grep -q Linux /etc/redhat-release; then
  distribution_version=$(cut -d " " -f 4 /etc/redhat-release)
else
  distribution_version=$(cut -d " " -f 3 /etc/redhat-release)
fi
distribution_version=$(echo ${distribution_version} | sed -e 's/\..*$//g')

architecture="$(arch)"
case "${architecture}" in
  i*86)
    architecture=i386
    ;;
esac

run yum groupinstall -y "Development Tools"
run yum install -y rpm-build rpmdevtools tar wget

if [ -x /usr/bin/rpmdev-setuptree ]; then
  rm -rf .rpmmacros
  run rpmdev-setuptree
else
  run cat <<EOM > ~/.rpmmacros
%_topdir ${HOME}/rpmbuild
EOM
  run mkdir -p ~/rpmbuild/SOURCES
  run mkdir -p ~/rpmbuild/SPECS
  run mkdir -p ~/rpmbuild/BUILD
  run mkdir -p ~/rpmbuild/RPMS
  run mkdir -p ~/rpmbuild/SRPMS
fi

repository="/vagrant/repositories/${distribution}/${distribution_version}"
rpm_dir="${repository}/${architecture}/Packages"
srpm_dir="${repository}/source/SRPMS"
run mkdir -p "${rpm_dir}" "${srpm_dir}"

rpmbuild_options=""

# for debug
# rpmbuild_options="${rpmbuild_options} --define 'optflags -O0 -g3'"

cd

run cp /vagrant/tmp/${PACKAGE}-${VERSION}.* rpmbuild/SOURCES/
run cp /vagrant/tmp/${distribution}/*.spec rpmbuild/SPECS/

package_name=$(cd rpmbuild/SPECS; echo *.spec | sed -e 's/\.spec$//g')

case ${distribution} in
  fedora)
    USE_MYSQLSERVICES_COMPAT=yes
    run yum install -y mariadb-devel
    ;;
  centos)
    case ${package_name} in
      mysql55-${PACKAGE})
	USE_MYSQLSERVICES_COMPAT=yes
        run yum install -y scl-utils-build
        if [ ${distribution_version} = 6 ]; then
	  run yum install -y centos-release-SCL
        fi
        run yum install -y mysql55-mysql-devel mysql55-build
	;;
      mysql56-community-${PACKAGE})
        release_rpm=mysql-community-release-el${distribution_version}-5.noarch.rpm
        run yum -y install http://repo.mysql.com/${release_rpm}
        run yum -y install mysql-community-devel
        ;;
      mariadb-${PACKAGE})
        run yum -y install mariadb-devel
	;;
    esac

    release_rpm=groonga-release-1.1.0-1.noarch.rpm
    wget http://packages.groonga.org/${distribution}/${release_rpm}
    run rpm -U ${release_rpm}
    rm -f ${release_rpm}
    run yum makecache
    ;;
esac
run yum install -y ${DEPENDED_PACKAGES}

if [ "${USE_MYSQLSERVICES_COMPAT}" = "yes" ]; then
  rpmbuild_options="$rpmbuild_options --define 'mroonga_configure_options --with-libmysqlservices-compat'"
fi

run eval rpmbuild -ba ${rpmbuild_options} rpmbuild/SPECS/${package_name}.spec

run mv rpmbuild/RPMS/*/* "${rpm_dir}/"
run mv rpmbuild/SRPMS/* "${srpm_dir}/"
