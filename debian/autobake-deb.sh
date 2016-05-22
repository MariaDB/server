#!/bin/bash

# Build MariaDB .deb packages.
# Based on OurDelta .deb packaging scripts, which are in turn based on Debian
# MySQL packages.

# Exit immediately on any error
set -e

# Debug script and command lines
#set -x

# Don't run the mysql-test-run test suite as part of build.
# It takes a lot of time, and we will do a better test anyway in
# Buildbot, running the test suite from installed .debs on a clean VM.
export DEB_BUILD_OPTIONS="nocheck"

export MARIADB_OPTIONAL_DEBS=""

# Find major.minor version.
#
source ./VERSION
UPSTREAM="${MYSQL_VERSION_MAJOR}.${MYSQL_VERSION_MINOR}.${MYSQL_VERSION_PATCH}${MYSQL_VERSION_EXTRA}"
RELEASE_EXTRA=""

RELEASE_NAME=""
PATCHLEVEL="+maria"
LOGSTRING="MariaDB build"

# Look up distro-version specific stuff.

CODENAME="$(lsb_release -sc)"

# add libcrack2 (>= 2.9.0) as a build dependency
# but only where the distribution can possibly satisfy it
if apt-cache madison cracklib2|grep 'cracklib2 *| *2\.[0-8]\.' >/dev/null 2>&1
then
  # Anything in MARIADB_OPTIONAL_DEBS is omitted from the resulting
  # packages by snipped in rules file
  MARIADB_OPTIONAL_DEBS="${MARIADB_OPTIONAL_DEBS} cracklib-password-check-10.1"
  sed -i -e "/\\\${MAYBE_LIBCRACK}/d" debian/control
else
  MAYBE_LIBCRACK='libcrack2-dev (>= 2.9.0),'
  sed -i -e "s/\\\${MAYBE_LIBCRACK}/${MAYBE_LIBCRACK}/g" debian/control
fi

# Adjust changelog, add new version.
#
echo "Incrementing changelog and starting build scripts"

dch -b -D ${CODENAME} -v "${UPSTREAM}${PATCHLEVEL}-${RELEASE_NAME}${RELEASE_EXTRA:+-${RELEASE_EXTRA}}1~${CODENAME}" "Automatic build with ${LOGSTRING}."

echo "Creating package version ${UPSTREAM}${PATCHLEVEL}-${RELEASE_NAME}${RELEASE_EXTRA:+-${RELEASE_EXTRA}}1~${CODENAME} ... "

# Build the package.
# Pass -I so that .git and other unnecessary temporary and source control files
# will be ignored by dpkg-source when createing the tar.gz source package
fakeroot dpkg-buildpackage -us -uc -I

[ -e debian/autorm-file ] && rm -vf `cat debian/autorm-file`

echo "Build complete"

# end of autobake script
