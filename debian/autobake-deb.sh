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
    cat storage/columnstore/columnstore/debian/control >> debian/control
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

architecture=$(dpkg-architecture -q DEB_BUILD_ARCH)

LSBID="$(lsb_release -si  | tr '[:upper:]' '[:lower:]')"
LSBVERSION="$(lsb_release -sr | sed -e "s#\.##g")"
LSBNAME="$(lsb_release -sc)"

if [ -z "${LSBID}" ]
then
    LSBID="unknown"
fi
case "${LSBNAME}" in
  buster)
    ;&
  bullseye|bookworm)
    # mariadb-plugin-rocksdb in control is 4 arches covered by the distro rocksdb-tools
    # so no removal is necessary.
    ;&
  sid)
    # should always be empty here.
    # need to match here to avoid the default Error however
    ;;
  # UBUNTU
  focal)
    ;&
  impish|jammy|kinetic)
    # mariadb-plugin-rocksdb s390x not supported by us (yet)
    # ubuntu doesn't support mips64el yet, so keep this just
    # in case something changes.
    if [[ ! "$architecture" =~ amd64|arm64|ppc64el|s390x ]]
    then
      remove_rocksdb_tools
    fi
    ;;
  *)
    echo "Error - unknown release codename $LSBNAME" >&2
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
VERSION="${EPOCH}${UPSTREAM}${PATCHLEVEL}~${LSBID:0:3}${LSBVERSION}"

dch -b -D ${LSBNAME} -v "${VERSION}" "Automatic build with ${LOGSTRING}." --controlmaint

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
