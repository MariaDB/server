#!/bin/bash
#
# Build MariaDB .deb packages for test and release at mariadb.org
#

# Exit immediately on any error
set -e

# On Buildbot, don't run the mysql-test-run test suite as part of build.
# It takes a lot of time, and we will do a better test anyway in
# Buildbot, running the test suite from installed .debs on a clean VM.
# On Travis-CI we want to simulate the full build, including tests.
# Also on Travis-CI it is useful not to override the DEB_BUILD_OPTIONS
# at this stage at all.
if [[ ! $TRAVIS ]]
then
  export DEB_BUILD_OPTIONS="nocheck"
fi

# Travis-CI optimizations
if [[ $TRAVIS ]]
then
  # On Travis-CI, the log must stay under 4MB so make the build less verbose
  sed -i -e '/Add support for verbose builds/,+2d' debian/rules

  # Don't include test suite package on Travis-CI to make the build time shorter
  sed '/Package: mariadb-test-data/,/^$/d' -i debian/control
  sed '/Package: mariadb-test/,/^$/d' -i debian/control
  sed '/Package: mariadb-plugin-tokudb/,/^$/d' -i debian/control
  sed '/Package: mariadb-plugin-mroonga/,/^$/d' -i debian/control
  sed '/Package: mariadb-plugin-spider/,/^$/d' -i debian/control
  sed '/Package: mariadb-plugin-oqgraph/,/^$/d' -i debian/control
  export MYSQL_COMPILER_LAUNCHER=ccache
  sed 's|-DDEB|-DPLUGIN_TOKUDB=NO -DPLUGIN_MROONGA=NO -DPLUGIN_SPIDER=NO -DPLUGIN_OQGRAPH=NO -DPLUGIN_PERFSCHEMA=NO -WITH_EMBEDDED_SERVER=OFF -DDEB|' -i debian/rules
fi


# Look up distro-version specific stuff
#
# Always keep the actual packaging as up-to-date as possible following the latest
# Debian policy and targetting Debian Sid. Then case-by-case run in autobake-deb.sh
# tests for backwards compatibility and strip away parts on older builders.

# If iproute2 is not available (before Debian Jessie and Ubuntu Trusty)
# fall back to the old iproute package.
if ! apt-cache madison iproute2 | grep 'iproute2 *|' >/dev/null 2>&1
then
 sed 's/iproute2/iproute/' -i debian/control
fi

# If libcrack2 (>= 2.9.0) is not available (before Debian Jessie and Ubuntu Trusty)
# clean away the cracklib stanzas so the package can build without them.
if ! apt-cache madison libcrack2-dev | grep 'libcrack2-dev *| *2\.9' >/dev/null 2>&1
then
  sed '/libcrack2-dev/d' -i debian/control
  sed '/Package: mariadb-plugin-cracklib/,/^$/d' -i debian/control
fi

# If libpcre3-dev (>= 2:8.35-3.2~) is not available (before Debian Jessie or Ubuntu Wily)
# clean away the PCRE3 stanzas so the package can build without them.
# Update check when version 2:8.40 or newer is available.
if ! apt-cache madison libpcre3-dev | grep 'libpcre3-dev *| *2:8\.3[2-9]' >/dev/null 2>&1
then
  sed '/libpcre3-dev/d' -i debian/control
fi

# If libsystemd-dev is not available (before Debian Jessie or Ubuntu Wily)
# clean away the systemd stanzas so the package can build without them.
if ! apt-cache madison libsystemd-dev | grep 'libsystemd-dev' >/dev/null 2>&1
then
  sed '/dh-systemd/d' -i debian/control
  sed '/libsystemd-dev/d' -i debian/control
  sed 's/ --with systemd//' -i debian/rules
  sed '/systemd/d' -i debian/rules
  sed '/\.service/d' -i debian/rules
  sed '/galera_new_cluster/d' -i debian/mariadb-server-10.2.install
  sed '/galera_recovery/d' -i debian/mariadb-server-10.2.install
  sed '/mariadb-service-convert/d' -i debian/mariadb-server-10.2.install
fi

# Convert gcc version to numberical value. Format is Mmmpp where M is Major
# version, mm is minor version and p is patch.
# -dumpfullversion & -dumpversion to make it uniform across old and new (>=7)
GCCVERSION=$(gcc -dumpfullversion -dumpversion | sed -e 's/\.\([0-9][0-9]\)/\1/g' \
                                                     -e 's/\.\([0-9]\)/0\1/g'     \
						     -e 's/^[0-9]\{3,4\}$/&00/')
# Don't build rocksdb package if gcc version is less than 4.8 or we are running on
# x86 32 bit.
if [[ $GCCVERSION -lt 40800 ]] || [[ $(arch) =~ i[346]86 ]]
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

# From Debian Stretch/Ubuntu Bionic onwards dh-systemd is just an empty
# transitional metapackage and the functionality was merged into debhelper.
# In Ubuntu Hirsute is was completely removed, so it can't be referenced anymore.
# Keep using it only on Debian Jessie and Ubuntu Xenial.
if apt-cache madison dh-systemd | grep 'dh-systemd' >/dev/null 2>&1
then
  sed 's/debhelper (>= 9.20160709~),/debhelper (>= 9), dh-systemd,/' -i debian/control
fi

# From Debian Bullseye/Ubuntu Hirsute there is no longer any libreadline-gplv2-dev
# available and it was replaced with libedit-dev in commit
# https://github.com/MariaDB/server/commit/5cdf245d7e2ab339ad3dba0dbbb591ab80e0dad0
# This commit was however only applied on 10.5 and newer branches. Since we still
# release MariaDB 10.2, 10.3 and 10.4 on new Debian and Ubuntu releases, this change
# was partially backported but at the same time using the code below we ensure that
# binary releases of old MariaDB versions for old distro versions keep using the old
# dependency, as adding a new dependency in a new release is otherwise forbidden by policy.
if apt-cache madison libreadline-gplv2-dev | grep 'libreadline-gplv2-dev' >/dev/null 2>&1
then
  sed 's/libedit-dev,/libreadline-gplv2-dev,/' -i debian/control
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

dch -b -D ${CODENAME} -v "${EPOCH}${UPSTREAM}${PATCHLEVEL}~${CODENAME}" "Automatic build with ${LOGSTRING}." --controlmaint

echo "Creating package version ${EPOCH}${UPSTREAM}${PATCHLEVEL}~${CODENAME} ... "

# On Travis CI, use -b to build binary only packages as there is no need to
# waste time on generating the source package.
if [[ $TRAVIS ]]
then
  BUILDPACKAGE_FLAGS="-b"
fi

# Build the package
# Pass -I so that .git and other unnecessary temporary and source control files
# will be ignored by dpkg-source when creating the tar.gz source package.
fakeroot dpkg-buildpackage -us -uc -I $BUILDPACKAGE_FLAGS

# If the step above fails due to missing dependencies, you can manually run
#   sudo mk-build-deps debian/control -r -i

# Don't log package contents on Travis-CI to save time and log size
if [[ ! $TRAVIS ]]
then
  echo "List package contents ..."
  cd ..
  for package in `ls *.deb`
  do
    echo $package | cut -d '_' -f 1
    dpkg-deb -c $package | awk '{print $1 " " $2 " " $6}' | sort -k 3
    echo "------------------------------------------------"
  done
fi

echo "Build complete"
