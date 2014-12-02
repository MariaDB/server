#!/bin/sh

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

run rpm -ivh http://packages.groonga.org/centos/groonga-release-1.1.0-1.noarch.rpm
run yum makecache

run yum groupinstall -y "Development Tools"
run yum install -y rpm-build rpmdevtools tar ${DEPENDED_PACKAGES}

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

# for debug
# rpmbuild_options="$rpmbuild_options --define 'optflags -O0 -g3'"

cd

run cp /vagrant/tmp/${PACKAGE}-${VERSION}.* rpmbuild/SOURCES/
run cp /vagrant/tmp/${distribution}/${PACKAGE}.spec rpmbuild/SPECS/

run rpmbuild -ba ${rpmbuild_options} rpmbuild/SPECS/${PACKAGE}.spec

run mv rpmbuild/RPMS/*/* "${rpm_dir}/"
run mv rpmbuild/SRPMS/* "${srpm_dir}/"
