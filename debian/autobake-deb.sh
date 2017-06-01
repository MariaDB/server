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
  sed '/Package: mariadb-test-data/,+28d' -i debian/control
  sed '/Package: mariadb-test/,+36d' -i debian/control
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
  sed '/Package: mariadb-plugin-cracklib/,+9d' -i debian/control
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
  sed '/galera_new_cluster/d' -i debian/mariadb-server-10.3.install
  sed '/galera_recovery/d' -i debian/mariadb-server-10.3.install
  sed '/mariadb-service-convert/d' -i debian/mariadb-server-10.3.install
fi

# Convert gcc version to numberical value. Format is Mmmpp where M is Major
# version, mm is minor version and p is patch.
GCCVERSION=$(gcc -dumpversion | sed -e 's/\.\([0-9][0-9]\)/\1/g' -e 's/\.\([0-9]\)/0\1/g' -e 's/^[0-9]\{3,4\}$/&00/')
# Don't build rocksdb package if gcc version is less than 4.8 or we are running on
# x86 32 bit.
if [[ $GCCVERSION -lt 40800 ]] || [[ $(arch) =~ i[346]86 ]]
then
  sed '/Package: mariadb-plugin-rocksdb/,+11d' -i debian/control
fi
if [[ $GCCVERSION -lt 40800 ]]
then
  sed '/Package: mariadb-plugin-aws-key-management-10.2/,+13d' -i debian/control
fi


# Adjust changelog, add new version
echo "Incrementing changelog and starting build scripts"

# Find major.minor version
source ./VERSION
UPSTREAM="${MYSQL_VERSION_MAJOR}.${MYSQL_VERSION_MINOR}.${MYSQL_VERSION_PATCH}${MYSQL_VERSION_EXTRA}"
PATCHLEVEL="+maria"
LOGSTRING="MariaDB build"
CODENAME="$(lsb_release -sc)"

dch -b -D ${CODENAME} -v "${UPSTREAM}${PATCHLEVEL}~${CODENAME}" "Automatic build with ${LOGSTRING}."

echo "Creating package version ${UPSTREAM}${PATCHLEVEL}~${CODENAME} ... "

# Build the package
# Pass -I so that .git and other unnecessary temporary and source control files
# will be ignored by dpkg-source when creating the tar.gz source package.
# Use -b to build binary only packages as there is no need to waste time on
# generating the source package.
fakeroot dpkg-buildpackage -us -uc -I -b

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
