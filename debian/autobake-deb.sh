#!/bin/bash
#
# Build MariaDB .deb packages for test and release at mariadb.org
#

# Exit immediately on any error
set -e

# This file is invocated from Buildbot and Travis-CI to build deb packages.
# As both of those CI systems have many parallel jobs that include different
# parts of the test suite, we don't need to run the mysql-test-run at all when
# building the deb packages here.
export DEB_BUILD_OPTIONS="nocheck $DEB_BUILD_OPTIONS"

# Travis-CI optimizations
if [[ $TRAVIS ]]
then
  # On Travis-CI, the log must stay under 4MB so make the build less verbose
  sed -i -e '/Add support for verbose builds/,+2d' debian/rules

  # Don't include test suite package on Travis-CI to make the build time shorter
  sed '/Package: mariadb-test-data/,+28d' -i debian/control
  sed '/Package: mariadb-test/,+36d' -i debian/control

  # Don't build the test package at all to save time and disk space
  sed 's|DINSTALL_MYSQLTESTDIR=share/mysql/mysql-test|DINSTALL_MYSQLTESTDIR=false|' -i debian/rules

  # Also skip building RocksDB and TokuDB to save even more time and disk space
  sed 's|-DDEB|-DWITHOUT_TOKUDB_STORAGE_ENGINE=true -DWITHOUT_MROONGA_STORAGE_ENGINE=true -DWITHOUT_ROCKSDB_STORAGE_ENGINE=true -DDEB|' -i debian/rules
fi


# Look up distro-version specific stuff
#
# Always keep the actual packaging as up-to-date as possible following the latest
# Debian policy and targeting Debian Sid. Then case-by-case run in autobake-deb.sh
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
# -dumpfullversion & -dumpversion to make it uniform across old and new (>=7)
GCCVERSION=$(gcc -dumpfullversion -dumpversion | sed -e 's/\.\([0-9][0-9]\)/\1/g' \
                                                     -e 's/\.\([0-9]\)/0\1/g'     \
						     -e 's/^[0-9]\{3,4\}$/&00/')
# Don't build rocksdb package if gcc version is less than 4.8 or we are running on
# x86 32 bit.
if [[ $GCCVERSION -lt 40800 ]] || [[ $(arch) =~ i[346]86 ]] || [[ $TRAVIS ]]
then
  sed '/Package: mariadb-plugin-rocksdb/,+14d' -i debian/control
fi

# AWS SDK requires c++11 -capable compiler.
# Minimal supported versions are g++ 4.8 and clang 3.3.
# AWS SDK also requires the build machine to have network access and git, so
# it cannot be part of the base version included in Linux distros, but a pure
# custom built plugin.
if [[ $GCCVERSION -gt 40800 ]] && [[ ! $TRAVIS ]] && ping -c 1 github.com
then
  cat <<EOF >> debian/control

Package: mariadb-plugin-aws-key-management
Architecture: any
Breaks: mariadb-aws-key-management-10.1,
        mariadb-aws-key-management-10.2
Replaces: mariadb-aws-key-management-10.1,
          mariadb-aws-key-management-10.2
Depends: libcurl3,
         mariadb-server-10.3,
         \${misc:Depends},
         \${shlibs:Depends}
Description: Amazon Web Service Key Management Service Plugin for MariaDB
 This encryption key management plugin gives an interface to the Amazon Web
 Services Key Management Service for managing encryption keys used for MariaDB
 data-at-rest encryption.
EOF
fi

# Mroonga, TokuDB never built on Travis CI anyway, see build flags above
if [[ $TRAVIS ]]
then
  sed -i -e "/Package: mariadb-plugin-tokudb/,+17d" debian/control
  sed -i -e "/Package: mariadb-plugin-mroonga/,+16d" debian/control
fi

# Adjust changelog, add new version
echo "Incrementing changelog and starting build scripts"

# Find major.minor version
source ./VERSION
UPSTREAM="${MYSQL_VERSION_MAJOR}.${MYSQL_VERSION_MINOR}.${MYSQL_VERSION_PATCH}${MYSQL_VERSION_EXTRA}"
PATCHLEVEL="+maria"
LOGSTRING="MariaDB build"
CODENAME="$(lsb_release -sc)"
EPOCH="1:"

dch -b -D ${CODENAME} -v "${EPOCH}${UPSTREAM}${PATCHLEVEL}~${CODENAME}" "Automatic build with ${LOGSTRING}."

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
