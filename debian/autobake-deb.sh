#!/bin/bash
#
# Build MariaDB .deb packages for test and release at mariadb.org
#

# Exit immediately on any error
set -e

# On Buildbot, don't run the mysql-test-run test suite as part of build.
# It takes a lot of time, and we will do a better test anyway in
# Buildbot, running the test suite from installed .debs on a clean VM.
export DEB_BUILD_OPTIONS="nocheck"

# Look up distro-version specific stuff
#
# Always keep the actual packaging as up-to-date as possible following the latest
# Debian policy and targetting Debian Sid. Then case-by-case run in autobake-deb.sh
# tests for backwards compatibility and strip away parts on older builders.

# Don't build rocksdb package on x86 32 bit.
if [[ $(arch) =~ i[346]86 ]]
then
  sed '/Package: mariadb-plugin-rocksdb/,/^$/d' -i debian/control
fi

## Skip TokuDB if arch is not amd64
if [[ ! $(dpkg-architecture -q DEB_BUILD_ARCH) =~ amd64 ]]
then
  sed '/Package: mariadb-plugin-tokudb/,/^$/d' -i debian/control
fi

# Always remove aws plugin, see -DNOT_FOR_DISTRIBUTION in CMakeLists.txt
sed '/Package: mariadb-plugin-aws-key-management-10.2/,/^$/d' -i debian/control

# Don't build cassandra package if thrift is not installed
if [[ ! -f /usr/local/include/thrift/Thrift.h && ! -f /usr/include/thrift/Thrift.h ]]
then
  sed '/Package: mariadb-plugin-cassandra/,/^$/d' -i debian/control
fi

# Adjust changelog, add new version
echo "Incrementing changelog and starting build scripts"

# Find major.minor version
source ./VERSION
UPSTREAM="${MYSQL_VERSION_MAJOR}.${MYSQL_VERSION_MINOR}.${MYSQL_VERSION_PATCH}${MYSQL_VERSION_EXTRA}"
PATCHLEVEL="+maria"
LOGSTRING="MariaDB build"
CODENAME="$(lsb_release -sc)"
VERNUM="$(lsb_release -sr)"
if [[ "${VERNUM%.*}" -ge 18 ]]; then
  EPOCH="1:"
fi

dch -b -D ${CODENAME} -v "${EPOCH}${UPSTREAM}${PATCHLEVEL}~${CODENAME}" "Automatic build with ${LOGSTRING}."

echo "Creating package version ${EPOCH}${UPSTREAM}${PATCHLEVEL}~${CODENAME} ... "

# Build the package
# Pass -I so that .git and other unnecessary temporary and source control files
# will be ignored by dpkg-source when creating the tar.gz source package.
fakeroot dpkg-buildpackage -us -uc -I $BUILDPACKAGE_FLAGS

# If the step above fails due to missing dependencies, you can manually run
#   sudo mk-build-deps debian/control -r -i

echo "List package contents ..."
cd ..
for package in `ls *.deb`
do
  echo $package | cut -d '_' -f 1
  dpkg-deb -c $package | awk '{print $1 " " $2 " " $6}' | sort -k 3
  echo "------------------------------------------------"
done

echo "Build complete"
