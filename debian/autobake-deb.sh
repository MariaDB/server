#!/bin/bash

# Build MariaDB .deb packages.
# Based on OurDelta .deb packaging scripts, which are in turn based on Debian
# MySQL packages.

# Exit immediately on any error
set -e

# Debug script and command lines
#set -x

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

# Don't include test suite package on Travis-CI to make the build time shorter
if [[ $TRAVIS ]]
then
  sed '/Package: mariadb-test-data/,+26d' -i debian/control
  sed '/Package: mariadb-test/,+34d' -i debian/control
fi

export MARIADB_OPTIONAL_DEBS=""

# Find major.minor version.
#
source ./VERSION
UPSTREAM="${MYSQL_VERSION_MAJOR}.${MYSQL_VERSION_MINOR}.${MYSQL_VERSION_PATCH}${MYSQL_VERSION_EXTRA}"
PATCHLEVEL="+maria"
LOGSTRING="MariaDB build"

# Look up distro-version specific stuff.
# Always keep the actual packaging as up-to-date as possible following the latest
# Debian policy and targetting Debian Sid. Then case-by-case run in autobake-deb.sh
# tests for backwards compatibility and strip away parts on older builders.

CODENAME="$(lsb_release -sc)"

# If libcrack2 (>= 2.9.0) is not available (before Debian Jessie and Ubuntu Trusty)
# clean away the cracklib stanzas so the package can build without them.
if ! apt-cache madison libcrack2-dev | grep 'libcrack2-dev *| *2\.9' >/dev/null 2>&1
then
  sed '/libcrack2-dev/d' -i debian/control
  sed '/Package: mariadb-plugin-cracklib/,+10d' -i debian/control
fi

# If libpcre3-dev (>= 2:8.35-3.2~) is not available (before Debian Jessie or Ubuntu Wily)
# clean away the PCRE3 stanzas so the package can build without them.
# Update check when version 2:8.40 or newer is available.
if ! apt-cache madison libpcre3-dev | grep 'libpcre3-dev *| *2:8\.3[2-9]' >/dev/null 2>&1
then
  sed '/libpcre3-dev/d' -i debian/control
fi

# On Travis-CI, the log must stay under 4MB so make the build less verbose
if [[ $TRAVIS ]]
then
  sed -i -e '/Add support for verbose builds/,+2d' debian/rules
fi


# Adjust changelog, add new version.
#
echo "Incrementing changelog and starting build scripts"

dch -b -D ${CODENAME} -v "${UPSTREAM}${PATCHLEVEL}~${CODENAME}" "Automatic build with ${LOGSTRING}."

echo "Creating package version ${UPSTREAM}${PATCHLEVEL}~${CODENAME} ... "

# Build the package.
# Pass -I so that .git and other unnecessary temporary and source control files
# will be ignored by dpkg-source when createing the tar.gz source package
# Use -b to build binary only packages as there is no need to waste time on
# generating the source package.
fakeroot dpkg-buildpackage -us -uc -I -b

[ -e debian/autorm-file ] && rm -vf `cat debian/autorm-file`

echo "Build complete"

# end of autobake script
