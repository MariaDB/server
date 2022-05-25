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

# On Buildbot, don't run the mysql-test-run test suite as part of build.
# It takes a lot of time, and we will do a better test anyway in
# Buildbot, running the test suite from installed .debs on a clean VM.
export DEB_BUILD_OPTIONS="nocheck $DEB_BUILD_OPTIONS"

source ./VERSION

# General CI optimizations to keep build output smaller
if [[ $GITLAB_CI ]]
then
  # On Gitlab the output log must stay under 4MB so make the
  # build less verbose
  sed '/Add support for verbose builds/,/^$/d' -i debian/rules
elif [ -d storage/columnstore/columnstore/debian ]
then
  # ColumnStore is explicitly disabled in the native Debian build, so allow it
  # now when build is triggered by autobake-deb.sh (MariaDB.org) and when the
  # build is not running on Travis or Gitlab-CI
  sed '/-DPLUGIN_COLUMNSTORE=NO/d' -i debian/rules
  # Take the files and part of control from MCS directory
  if [ ! -f debian/mariadb-plugin-columnstore.install ]
  then
    cp -v storage/columnstore/columnstore/debian/mariadb-plugin-columnstore.* debian/
    echo >> debian/control
    sed "s/-10.6//" <storage/columnstore/columnstore/debian/control >> debian/control
  fi
fi

# Look up distro-version specific stuff
#
# Always keep the actual packaging as up-to-date as possible following the latest
# Debian policy and targeting Debian Sid. Then case-by-case run in autobake-deb.sh
# tests for backwards compatibility and strip away parts on older builders.

remove_rocksdb_tools()
{
  sed '/rocksdb-tools/d' -i debian/control
  sed '/sst_dump/d' -i debian/not-installed
  if ! grep -q sst_dump debian/mariadb-plugin-rocksdb.install
  then
    echo "usr/bin/sst_dump" >> debian/mariadb-plugin-rocksdb.install
  fi
}

replace_uring_with_aio()
{
  sed 's/liburing-dev/libaio-dev/g' -i debian/control
  sed -e '/-DIGNORE_AIO_CHECK=YES/d' \
      -e '/-DWITH_URING=yes/d' -i debian/rules
}

disable_pmem()
{
  sed '/libpmem-dev/d' -i debian/control
  sed '/-DWITH_PMEM=yes/d' -i debian/rules
}

disable_libfmt()
{
  # 0.7+ required
  sed '/libfmt-dev/d' -i debian/control
}

architecture=$(dpkg-architecture -q DEB_BUILD_ARCH)

CODENAME="$(lsb_release -sc)"
case "${CODENAME}" in
  stretch)
    # MDEV-16525 libzstd-dev-1.1.3 minimum version
    sed -e '/libzstd-dev/d' \
        -e 's/libcurl4/libcurl3/g' -i debian/control
    remove_rocksdb_tools
    disable_pmem
    ;&
  buster)
    disable_libfmt
    replace_uring_with_aio
    if [ ! "$architecture" = amd64 ]
    then
      disable_pmem
    fi
    ;&
  bullseye|bookworm)
    # mariadb-plugin-rocksdb in control is 4 arches covered by the distro rocksdb-tools
    # so no removal is necessary.
    if [[ ! "$architecture" =~ amd64|arm64|ppc64el ]]
    then
      disable_pmem
    fi
    if [[ ! "$architecture" =~ amd64|arm64|armel|armhf|i386|mips64el|mipsel|ppc64el|s390x ]]
    then
      replace_uring_with_aio
    fi
    ;&
  sid)
    # should always be empty here.
    # need to match here to avoid the default Error however
    ;;
    # UBUNTU
  bionic)
    remove_rocksdb_tools
    [ "$architecture" != amd64 ] && disable_pmem
    ;&
  focal)
    replace_uring_with_aio
    disable_libfmt
    ;&
  impish|jammy)
    # mariadb-plugin-rocksdb s390x not supported by us (yet)
    # ubuntu doesn't support mips64el yet, so keep this just
    # in case something changes.
    if [[ ! "$architecture" =~ amd64|arm64|ppc64el|s390x ]]
    then
      remove_rocksdb_tools
    fi
    if [[ ! "$architecture" =~ amd64|arm64|ppc64el ]]
    then
      disable_pmem
    fi
    if [[ ! "$architecture" =~ amd64|arm64|armhf|ppc64el|s390x ]]
    then
      replace_uring_with_aio
    fi
    ;;
  *)
    echo "Error - unknown release codename $CODENAME" >&2
    exit 1
esac

if [ -n "${AUTOBAKE_PREP_CONTROL_RULES_ONLY:-}" ]
then
  exit 0
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

# Don't log package contents on Gitlab-CI to save time and log size
if [[ ! $GITLAB_CI ]]
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
