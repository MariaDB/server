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

# shellcheck source=/dev/null
source ./VERSION

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

add_lsb_base_depends()
{
  # Make sure one can run this multiple times remove
  # lines 'sysvinit-utils' and 'lsb-base'.
  sed -e '/sysvinit-utils/d' -e '/lsb-base/d' -i debian/control
  # Add back lsb-base before lsof
  sed -e 's#lsof #lsb-base (>= 3.0-10),\n         lsof #' -i debian/control
}

replace_uring_with_aio()
{
  sed 's/liburing-dev/libaio-dev/g' -i debian/control
  sed -e '/-DIGNORE_AIO_CHECK=ON/d' \
      -e '/-DWITH_URING=ON/d' -i debian/rules
}

disable_libfmt()
{
  # 7.0+ required
  sed '/libfmt-dev/d' -i debian/control
}

remove_package_notes()
{
  # binutils >=2.39 + distro makefile /usr/share/debhelper/dh_package_notes/package-notes.mk
  sed -e '/package.notes/d' -i debian/rules debian/control
}

architecture=$(dpkg-architecture -q DEB_BUILD_ARCH)
uname_machine=$(uname -m)

# Parse release name and number from Linux standard base release
# Example:
#   $ lsb_release -a
#   No LSB modules are available.
#   Distributor ID:	Debian
#   Description:	Debian GNU/Linux bookworm/sid
#   Release:	n/a
#   Codename:	n/a
LSBID="$(lsb_release -si  | tr '[:upper:]' '[:lower:]')"
LSBVERSION="$(lsb_release -sr | sed -e "s/\.//g")"
LSBNAME="$(lsb_release -sc)"

# If 'n/a', assume 'sid'
if [ "${LSBVERSION}" == "n/a" ] || [ "${LSBNAME}" == "n/a" ]
then
  LSBVERSION="sid"
  LSBNAME="sid"
fi

# If not known, use 'unknown' in .deb version identifier
if [ -z "${LSBID}" ]
then
  LSBID="unknown"
fi

case "${LSBNAME}"
in
  # Debian
  "buster")
    disable_libfmt
    replace_uring_with_aio
    ;&
  "bullseye")
    add_lsb_base_depends
    remove_package_notes
    ;&
  "bookworm")
    # mariadb-plugin-rocksdb in control is 4 arches covered by the distro rocksdb-tools
    # so no removal is necessary.
    if [[ ! "$architecture" =~ amd64|arm64|armel|armhf|i386|mips64el|mipsel|ppc64el|s390x ]]
    then
      replace_uring_with_aio
    fi
    ;&
  "trixie"|"forky"|"sid")
    # The default packaging should always target Debian Sid, so in this case
    # there is intentionally no customizations whatsoever.
    ;;
  # Ubuntu
  "focal")
    replace_uring_with_aio
    disable_libfmt
    ;&
  "jammy"|"kinetic")
    add_lsb_base_depends
    remove_package_notes
    ;&
  "lunar"|"mantic")
    if [[ ! "$architecture" =~ amd64|arm64|armhf|ppc64el|s390x ]]
    then
      replace_uring_with_aio
    fi
    ;&
  "noble"|"oracular"|"plucky"|"questing")
    # mariadb-plugin-rocksdb s390x not supported by us (yet)
    # ubuntu doesn't support mips64el yet, so keep this just
    # in case something changes.
    if [[ ! "$architecture" =~ amd64|arm64|ppc64el|s390x ]]
    then
      remove_rocksdb_tools
    fi
    ;;
  *)
    echo "Error: Unknown release '$LSBNAME'" >&2
    exit 1
esac

# General CI optimizations to keep build output smaller
if [[ $GITLAB_CI ]]
then
  # On Gitlab the output log must stay under 4MB so make the
  # build less verbose
  sed '/Add support for verbose builds/,/^$/d' -i debian/rules
elif [[ -d storage/columnstore/columnstore/debian ]] && [[ "$LSBNAME" = !(buster|bionic) ]]
then
  # ColumnStore is explicitly disabled in the native Debian build. Enable it
  # now when build is triggered by autobake-deb.sh (MariaDB.org) and when the
  # build is not running on Gitlab-CI.
  sed '/-DPLUGIN_COLUMNSTORE=NO/d' -i debian/rules
  # Take the files and part of control from MCS directory
  if [[ ! -f debian/mariadb-plugin-columnstore.install ]]
  then
    cp -v storage/columnstore/columnstore/debian/mariadb-plugin-columnstore.* debian/
    echo >> debian/control
    cat storage/columnstore/columnstore/debian/control >> debian/control
  fi
fi

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

dch -b -D "${LSBNAME}" -v "${VERSION}" "Automatic build with ${LOGSTRING}." --controlmaint

echo "Creating package version ${VERSION} ... "

BUILDPACKAGE_DPKGCMD=()

# Fakeroot test
if fakeroot true; then
  BUILDPACKAGE_DPKGCMD+=( "fakeroot" "--" )
fi

# Use eatmydata is available to build faster with less I/O, skipping fsync()
# during the entire build process (safe because a build can always be restarted)
if command -v eatmydata > /dev/null
then
  BUILDPACKAGE_DPKGCMD+=("eatmydata")
fi

# If running autobake-debs.sh inside docker/podman host machine which
# has 64 bits cpu but container image is 32 bit make sure that we set
# correct arch with linux32 for 32 bit enviroment
if [ "$architecture" = "i386" ] && [ "$uname_machine" = "x86_64" ]
then
  BUILDPACKAGE_DPKGCMD+=("linux32")
fi

BUILDPACKAGE_DPKGCMD+=("dpkg-buildpackage")

# Using dpkg-buildpackage args
# -us Allow unsigned sources
# -uc Allow unsigned changes
# -I  Tar ignore
BUILDPACKAGE_DPKGCMD+=(-us -uc -I)

# There can be also extra flags that are appended to args
if [ -n "$BUILDPACKAGE_FLAGS" ]
then
  read -ra BUILDPACKAGE_TMP_ARGS <<< "$BUILDPACKAGE_FLAGS"
  BUILDPACKAGE_DPKGCMD+=( "${BUILDPACKAGE_TMP_ARGS[@]}" )
fi

# Build the package
# Pass -I so that .git and other unnecessary temporary and source control files
# will be ignored by dpkg-source when creating the tar.gz source package.
"${BUILDPACKAGE_DPKGCMD[@]}"

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
    # shellcheck disable=SC2034
    dpkg-deb -c "$package" | while IFS=" " read -r col1 col2 col3 col4 col5 col6 col7 col8
    do
        echo "$col1 $col2 $col6 $col7 $col8" | sort -k 3
    done
    echo "------------------------------------------------"
  done
fi

echo "Build complete"
