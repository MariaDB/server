# Check if we can safely upgrade.  An upgrade is only safe if it's from one
# of our RPMs in the same version family.

installed=`rpm -q --whatprovides mysql-server 2> /dev/null`
if [ $? -eq 0 -a -n "$installed" ]; then
  installed=`echo "$installed"|sed -n 1p`
  vendor=`rpm -q --queryformat='%''{VENDOR}' "$installed" 2> /dev/null | sed 's/Monty Program AB/MariaDB Foundation/'`
  version=`rpm -q --queryformat='%''{VERSION}' "$installed" 2> /dev/null`
  myvendor='%{mysql_vendor}'
  myversion='%{mysqlversion}'

  old_family=`echo $version   | sed -n -e 's,^\([1-9][0-9]*\)\..*$,\1,p'`
  new_family=`echo $myversion | sed -n -e 's,^\([1-9][0-9]*\)\..*$,\1,p'`

  [ -z "$vendor" ] && vendor='<unknown>'
  [ -z "$old_family" ] && old_family="<unrecognized version $version>"
  [ -z "$new_family" ] && new_family="<bad package specification: version $myversion>"

  error_text=
  if [ "$vendor" != "$myvendor" ]; then
    error_text="$error_text
The current MariaDB server package is provided by a different
vendor ($vendor) than $myvendor.  Some files may be installed
to different locations, including log files and the service
startup script in %{_sysconfdir}/init.d/.
"
  fi

  if [ "$old_family" != "$new_family" ]; then
    error_text="$error_text
Upgrading directly from MariaDB or MySQL $old_family
to MariaDB $new_family may not be safe in all cases.
A manual dump and restore using mariadb-dump (or mysqldump) is recommended.
It is important to review the MariaDB manual's Upgrading section for
version-specific incompatibilities.
"
  fi

  if [ -n "$error_text" ]; then
    cat <<HERE >&2

******************************************************************
A MariaDB or MySQL server package ($installed) is installed.
$error_text
A manual upgrade is required.

- Ensure that you have a complete, working backup of your data and my.cnf
  files
- Shut down the MariaDB or MySQL server cleanly
- Remove the existing MariaDB or MySQL packages. Usually this command will
  list the packages you should remove:
  rpm -qa | grep -iE '^(mariadb|mysql)-'

  You may choose to use 'rpm --nodeps -ev <package-name>' to remove
  the package which contains the mysqlclient shared library.  The
  library will be reinstalled by the MariaDB-shared package.
- Install the new MariaDB packages supplied by $myvendor
- Ensure that the MariaDB server is started
- Run the 'mariadb-upgrade' program

This is a brief description of the upgrade process.  Important details
can be found in the MariaDB manual, in the Upgrading section.
******************************************************************
HERE
    exit 1
  fi
fi

# Create a MariaDB user and group. Do not report any problems if it already exists.
groupadd -r %{mysqld_group} 2> /dev/null || true
useradd -M -r --home %{mysqldatadir} --shell /sbin/nologin --comment "MariaDB server" --gid %{mysqld_group} %{mysqld_user} 2> /dev/null || true
# The user may already exist, make sure it has the proper group nevertheless (BUG#12823)
usermod --gid %{mysqld_group} %{mysqld_user} 2> /dev/null || true
