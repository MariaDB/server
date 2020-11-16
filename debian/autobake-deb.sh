#!/bin/bash
#
# Build MariaDB .deb packages for test and release at mariadb.org
#
# Purpose of this script:
# Always keep the actual packaging as up-to-date as possible following the latest
# Debian policy and targeting Debian Sid. Then case-by-case run in autobake-deb.sh
# tests for backwards compatibility and strip away parts on older builders or
# specfic build environments.

# Exit immediately on any error
set -e

# This file is invocated from Buildbot and Travis-CI to build deb packages.
# As both of those CI systems have many parallel jobs that include different
# parts of the test suite, we don't need to run the mysql-test-run at all when
# building the deb packages here.
export DEB_BUILD_OPTIONS="nocheck $DEB_BUILD_OPTIONS"

# Take the files and part of control from MCS directory
if [[ -d storage/columnstore/columnstore/debian ]]; then
  cp -v storage/columnstore/columnstore/debian/mariadb-plugin-columnstore.* debian/
  echo >> debian/control
  cat storage/columnstore/columnstore/debian/control >> debian/control
fi

# General CI optimizations to keep build output smaller
if [[ $TRAVIS ]] || [[ $GITLAB_CI ]]
then
  # On both Travis and Gitlab the output log must stay under 4MB so make the
  # build less verbose
  sed '/Add support for verbose builds/,/^$/d' -i debian/rules

  # MCOL-4149: ColumnStore builds are so slow and big that they must be skipped on
  # both Travis-CI and Gitlab-CI
  sed 's|-DPLUGIN_COLUMNSTORE=YES|-DPLUGIN_COLUMNSTORE=NO|' -i debian/rules
  sed "/Package: mariadb-plugin-columnstore/,/^$/d" -i debian/control
fi

# Don't build or try to put files in a package for selected plugins and compontents on Travis-CI
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
  sed "/ha_sphinx.so/d" -i debian/mariadb-server-10.6.install
  sed "/Package: libmariadbd19/,/^$/d" -i debian/control
  sed "/Package: libmariadbd-dev/,/^$/d" -i debian/control
fi

if [[ $(arch) =~ i[346]86 ]]
then
  sed "/Package: mariadb-plugin-rocksdb/,/^$/d" -i debian/control
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

# From Debian Buster/Ubuntu Bionic, libcurl4 replaces libcurl3.
if ! apt-cache madison libcurl4 | grep 'libcurl4' >/dev/null 2>&1
then
  sed 's/libcurl4/libcurl3/g' -i debian/control
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

dch -b -D "${CODENAME}" -v "${EPOCH}${UPSTREAM}${PATCHLEVEL}~${CODENAME}" "Automatic build with ${LOGSTRING}."

echo "Creating package version ${EPOCH}${UPSTREAM}${PATCHLEVEL}~${CODENAME} ... "

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
