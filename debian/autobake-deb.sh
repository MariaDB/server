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
  sed '/galera_new_cluster/d' -i debian/mariadb-server-10.5.install
  sed '/galera_recovery/d' -i debian/mariadb-server-10.5.install
  sed '/mariadb-service-convert/d' -i debian/mariadb-server-10.5.install
fi

# If libzstd-dev is not available (before Debian Stretch and Ubuntu Xenial)
# remove the dependency from server and RocksDB so it can build properly
if ! apt-cache madison libzstd-dev | grep 'libzstd-dev' >/dev/null 2>&1
then
  sed '/libzstd-dev/d' -i debian/control
fi

# If rocksdb-tools is not available (before Debian Buster and Ubuntu Disco)
# remove the dependency from the RocksDB plugin so it can install properly
# and instead ship the one built from MariaDB sources
if ! apt-cache madison rocksdb-tools | grep 'rocksdb-tools' >/dev/null 2>&1
then
  sed '/rocksdb-tools/d' -i debian/control
  sed '/sst_dump/d' -i debian/not-installed
  echo "usr/bin/sst_dump" >> debian/mariadb-plugin-rocksdb.install
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

# Always remove aws plugin, see -DNOT_FOR_DISTRIBUTION in CMakeLists.txt
sed '/Package: mariadb-plugin-aws-key-management-10.2/,/^$/d' -i debian/control

# Don't build cassandra package if thrift is not installed
if [[ ! -f /usr/local/include/thrift/Thrift.h && ! -f /usr/include/thrift/Thrift.h ]]
then
  sed '/Package: mariadb-plugin-cassandra/,/^$/d' -i debian/control
fi

sed -i -e "/Package: mariadb-plugin-tokudb/,/^$/d" debian/control

# If libpcre2-dev is not available (before Debian Stretch and Ubuntu Xenial)
# attempt to build using older libpcre3-dev (SIC!)
if ! apt-cache madison libpcre2-dev | grep --quiet 'libpcre2-dev'
then
  sed 's/libpcre2-dev/libpcre3-dev/' -i debian/control
fi

# Mroonga, TokuDB never built on Travis CI anyway, see build flags above
if [[ $TRAVIS ]]
then
  sed -i -e "/Package: mariadb-plugin-tokudb/,/^$/d" debian/control
  sed -i -e "/Package: mariadb-plugin-mroonga/,/^$/d" debian/control
  sed -i -e "/Package: mariadb-plugin-spider/,/^$/d" debian/control
  sed -i -e "/Package: mariadb-plugin-oqgraph/,/^$/d" debian/control
  sed -i -e "/usr\/lib\/mysql\/plugin\/ha_sphinx.so/d" debian/mariadb-server-10.5.install
  sed -i -e "/Package: libmariadbd-dev/,/^$/d" debian/control
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
    dpkg-deb -c $package | awk '{print $1 " " $2 " " $6 " " $7 " " $8}' | sort -k 3
    echo "------------------------------------------------"
  done
fi

echo "Build complete"
