#!/bin/bash
#
# Build MariaDB .deb packages for test and release at mariadb.org
#
# Purpose of this script:
# Always keep the actual packaging as up-to-date as possible following the latest
# Debian policy and targeting Debian Sid. Then case-by-case run in autobake-deb.sh
# tests for backwards compatibility and strip away parts on older builders or
# specific build environments.

# Exit immediately on any error
set -e

source ./VERSION

CODENAME="$(lsb_release -sc)"
case "${CODENAME}" in
	stretch)
		# MDEV-28022 libzstd-dev-1.1.3 minimum version
		sed -i -e '/libzstd-dev/d' debian/control
		;;
esac

# This file is invoked from Buildbot and Travis-CI to build deb packages.
# As both of those CI systems have many parallel jobs that include different
# parts of the test suite, we don't need to run the mysql-test-run at all when
# building the deb packages here.
export DEB_BUILD_OPTIONS="nocheck $DEB_BUILD_OPTIONS"

# General CI optimizations to keep build output smaller
if [[ $TRAVIS ]] || [[ $GITLAB_CI ]]
then
  # On both Travis and Gitlab the output log must stay under 4MB so make the
  # build less verbose
  sed '/Add support for verbose builds/,/^$/d' -i debian/rules
elif [ -d storage/columnstore/columnstore/debian ]
then
  # ColumnStore is explicitly disabled in the native Debian build, so allow it
  # now when build is triggered by autobake-deb.sh (MariaDB.org) and when the
  # build is not running on Travis or Gitlab-CI
  sed '/-DPLUGIN_COLUMNSTORE=NO/d' -i debian/rules
  # Take the files and part of control from MCS directory
  cp -v storage/columnstore/columnstore/debian/mariadb-plugin-columnstore.* debian/
  echo >> debian/control
  sed "s/10.6/${MYSQL_VERSION_MAJOR}.${MYSQL_VERSION_MINOR}/" <storage/columnstore/columnstore/debian/control >> debian/control
fi

# Don't build or try to put files in a package for selected plugins and components on Travis-CI
# in order to keep build small (in both duration and disk space)
if [[ $TRAVIS ]]
then
  # Test suite package not relevant on Travis-CI
  sed 's|DINSTALL_MYSQLTESTDIR=share/mysql/mysql-test|DINSTALL_MYSQLTESTDIR=false|' -i debian/rules
  sed '/Package: mariadb-test-data/,/^$/d' -i debian/control
  sed '/Package: mariadb-test$/,/^$/d' -i debian/control

  # Extra plugins such as Mroonga, Spider, OQgraph, Sphinx and the embedded build can safely be skipped
  sed 's|-DDEB|-DPLUGIN_MROONGA=NO -DPLUGIN_ROCKSDB=NO -DPLUGIN_SPIDER=NO -DPLUGIN_OQGRAPH=NO -DPLUGIN_PERFSCHEMA=NO -DPLUGIN_SPHINX=NO -DWITH_EMBEDDED_SERVER=OFF -DDEB|' -i debian/rules
  sed "/Package: mariadb-plugin-mroonga/,/^$/d" -i debian/control
  sed "/Package: mariadb-plugin-rocksdb/,/^$/d" -i debian/control
  sed "/Package: mariadb-plugin-spider/,/^$/d" -i debian/control
  sed "/Package: mariadb-plugin-oqgraph/,/^$/d" -i debian/control
  sed "/ha_sphinx.so/d" -i debian/mariadb-server-10.7.install
  sed "/Package: libmariadbd19/,/^$/d" -i debian/control
  sed "/Package: libmariadbd-dev/,/^$/d" -i debian/control
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

# If libcurl4 is not available (before Debian Buster and Ubuntu Bionic)
# use older libcurl3 instead
if ! apt-cache madison libcurl4 | grep 'libcurl4' >/dev/null 2>&1
then
  sed 's/libcurl4/libcurl3/g' -i debian/control
fi

# From Debian Bullseye/Ubuntu Groovy, liburing replaces libaio
if ! apt-cache madison liburing-dev | grep 'liburing-dev' >/dev/null 2>&1
then
  sed 's/liburing-dev/libaio-dev/g' -i debian/control
  sed '/-DIGNORE_AIO_CHECK=YES/d' -i debian/rules
  sed '/-DWITH_URING=yes/d' -i debian/rules
fi

# From Debian Buster/Ubuntu Focal onwards libpmem-dev is available
# Don't reference it when built in distro releases that lack it
if ! apt-cache madison libpmem-dev | grep 'libpmem-dev' >/dev/null 2>&1
then
  sed '/libpmem-dev/d' -i debian/control
  sed '/-DWITH_PMEM=yes/d' -i debian/rules
fi

# Debian stretch doesn't support the zstd version 1.1.3 required
# for RocksDB. zstd isn't enabled in Mroonga even though code exists
# for it. If someone happens to have a non-default zstd installed
# (not 1.1.2), assume its a backport and build with it.
if [ "$(lsb_release -sc)" = stretch ] && [ "$(apt-cache madison 'libzstd-dev' | grep -v 1.1.2)" = '' ]
then
  sed '/libzstd-dev/d' -i debian/control
fi

# Adjust changelog, add new version
echo "Incrementing changelog and starting build scripts"

# Find major.minor version
UPSTREAM="${MYSQL_VERSION_MAJOR}.${MYSQL_VERSION_MINOR}.${MYSQL_VERSION_PATCH}${MYSQL_VERSION_EXTRA}"
PATCHLEVEL="+maria"
LOGSTRING="MariaDB build"
EPOCH="1:"
VERSION="${EPOCH}${UPSTREAM}${PATCHLEVEL}~${CODENAME}"

dch -b -D "${CODENAME}" -v "${VERSION}" "Automatic build with ${LOGSTRING}." --controlmaint

echo "Creating package version ${VERSION} ... "

# On Travis CI and Gitlab-CI, use -b to build binary only packages as there is
# no need to waste time on generating the source package.
if [[ $TRAVIS ]]
then
  BUILDPACKAGE_FLAGS="-b"
fi

# Use eatmydata is available to build faster with less I/O, skipping fsync()
# during the entire build process (safe because a build can always be restarted)
if which eatmydata > /dev/null
then
  BUILDPACKAGE_PREPEND=eatmydata
fi

# Build the package
# Pass -I so that .git and other unnecessary temporary and source control files
# will be ignored by dpkg-source when creating the tar.gz source package.
fakeroot $BUILDPACKAGE_PREPEND dpkg-buildpackage -us -uc -I $BUILDPACKAGE_FLAGS

# If the step above fails due to missing dependencies, you can manually run
#   sudo mk-build-deps debian/control -r -i

# Don't log package contents on Travis-CI or Gitlab-CI to save time and log size
if [[ ! $TRAVIS ]] && [[ ! $GITLAB_CI ]]
then
  echo "List package contents ..."
  cd ..
  for package in *.deb
  do
    echo "$package" | cut -d '_' -f 1
    dpkg-deb -c "$package" | awk '{print $1 " " $2 " " $6 " " $7 " " $8}' | sort -k 3
    echo "------------------------------------------------"
  done
fi

echo "Build complete"
