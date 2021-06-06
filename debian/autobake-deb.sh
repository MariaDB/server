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
  sed -i -e '/Add support for verbose builds/,/^$/d' debian/rules

  # Don't include test suite package on Travis-CI to make the build time shorter
  sed '/Package: mariadb-test-data/,/^$/d' -i debian/control
  sed '/Package: mariadb-test$/,/^$/d' -i debian/control

  # Don't build the test package at all to save time and disk space
  sed 's|DINSTALL_MYSQLTESTDIR=share/mysql/mysql-test|DINSTALL_MYSQLTESTDIR=false|' -i debian/rules

  # Also skip building RocksDB and TokuDB to save even more time and disk space
  sed 's|-DDEB|-DPLUGIN_TOKUDB=NO -DPLUGIN_MROONGA=NO -DPLUGIN_ROCKSDB=NO -DPLUGIN_SPIDER=NO -DPLUGIN_OQGRAPH=NO -DPLUGIN_PERFSCHEMA=NO -DPLUGIN_SPHINX=NO -WITH_EMBEDDED_SERVER=OFF -DDEB|' -i debian/rules
fi

# Convert gcc version to numberical value. Format is Mmmpp where M is Major
# version, mm is minor version and p is patch.
# -dumpfullversion & -dumpversion to make it uniform across old and new (>=7)
GCCVERSION=$(gcc -dumpfullversion -dumpversion | sed -e 's/\.\([0-9][0-9]\)/\1/g' \
                                                     -e 's/\.\([0-9]\)/0\1/g'     \
                                                     -e 's/^[0-9]\{3,4\}$/&00/')

# Look up distro-version specific stuff
#
# Always keep the actual packaging as up-to-date as possible following the latest
# Debian policy and targeting Debian Sid. Then case-by-case run in autobake-deb.sh
# tests for backwards compatibility and strip away parts on older builders.

# From Debian Bullseye/Ubuntu Hirsute onwards libreadline is gone, so build with
# it only on older releases where it is still available. On Hirsute and newer,
# link with libedit. This is a 10.4 adaptation of 10.5 commit 5cdf245d7e.
if apt-cache madison libreadline-gplv2-dev | grep 'libreadline-gplv2-dev' >/dev/null 2>&1
then
  sed 's/libedit-dev,/libreadline-gplv2-dev,/' -i debian/control
fi

# If libzstd-dev is not available (before Debian Stretch and Ubuntu Xenial)
# remove the dependency from server and rocksdb so it can build properly
if ! apt-cache madison libzstd-dev | grep 'libzstd-dev' >/dev/null 2>&1
then
  sed '/libzstd-dev/d' -i debian/control
fi

# The binaries should be fully hardened by default. However TokuDB compilation seems to fail on
# Debian Jessie and older and on Ubuntu Xenial and older with the following error message:
#   /usr/bin/ld.bfd.real: /tmp/ccOIwjFo.ltrans0.ltrans.o: relocation R_X86_64_PC32 against symbol
#   `toku_product_name_strings' can not be used when making a shared object; recompile with -fPIC
# Therefore we need to disable PIE on those releases using gcc as proxy for detection.
if [[ $GCCVERSION -lt 60000 ]]
then
  sed 's/hardening=+all$/hardening=+all,-pie/' -i debian/rules
fi

# Don't build rocksdb package if gcc version is less than 4.8 or we are running on
# x86 32 bit.
if [[ $GCCVERSION -lt 40800 ]] || [[ $(arch) =~ i[346]86 ]] || [[ $TRAVIS ]]
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

# Mroonga, TokuDB never built on Travis CI anyway, see build flags above
if [[ $TRAVIS ]]
then
  sed -i -e "/Package: mariadb-plugin-tokudb/,/^$/d" debian/control
  sed -i -e "/Package: mariadb-plugin-mroonga/,/^$/d" debian/control
  sed -i -e "/Package: mariadb-plugin-spider/,/^$/d" debian/control
  sed -i -e "/Package: mariadb-plugin-oqgraph/,/^$/d" debian/control
  sed -i -e "/usr\/lib\/mysql\/plugin\/ha_sphinx.so/d" debian/mariadb-server-10.4.install
  sed -i -e "/Package: libmariadbd-dev/,/^$/d" debian/control
fi

# Adjust changelog, add new version
echo "Incrementing changelog and starting build scripts"

# Find major.minor version
source ./VERSION
UPSTREAM="${MYSQL_VERSION_MAJOR}.${MYSQL_VERSION_MINOR}.${MYSQL_VERSION_PATCH}${MYSQL_VERSION_EXTRA}"
PATCHLEVEL="+maria"
LOGSTRING="MariaDB build"
CODENAME="$(lsb_release -sc || true)"  # If lsb_release is not installed, stay empty
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
fakeroot dpkg-buildpackage -us -uc -I $BUILDPACKAGE_FLAGS -j$(nproc)

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
