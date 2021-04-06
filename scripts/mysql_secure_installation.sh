#!/bin/sh

# Copyright (c) 2002, 2016, Oracle and/or its affiliates. All rights reserved.
# Copyright (c) 2021, MariaDB Foundation
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA

config=".my.cnf.$$"
command=".mysql.$$"
output=".my.output.$$"

trap "interrupt" 1 2 3 6 15

args=
user=""
password=""
host=
set_from_cli=0
emptyuser=0
echo_n=
echo_c=
basedir=
defaults_file=
defaults_extra_file=
no_defaults=

parse_arg()
{
  echo "$1" | sed -e 's/^[^=]*=//'
}

parse_arguments()
{
  # We only need to pass arguments through to the server if we don't
  # handle them here.  So, we collect unrecognized options (passed on
  # the command line) into the args variable.
  pick_args=
  if test "$1" = PICK-ARGS-FROM-ARGV
  then
    pick_args=1
    shift
  fi

  for arg
  do
  val=$(parse_arg "$arg")
    case "$arg" in
      --basedir=*) basedir="$val";;
      --defaults-file=*) defaults_file="$val" ;;
      --defaults-extra-file=*) defaults_extra_file="$val" ;;
      --no-defaults) no_defaults="$val" ;;
      *)
        if test -n "$pick_args"
        then
          # This sed command makes sure that any special chars are quoted,
          # so the arg gets passed exactly to the server.
          # XXX: This is broken; true fix requires using eval and proper
          # quoting of every single arg ($basedir, $ldata, etc.)
          #args="$args "`echo "$arg" | sed -e 's,\([^a-zA-Z0-9_.-]\),\\\\\1,g'`
          case $arg in
            --user=*) user="$val" set_from_cli=1;;
            --password=*) password="$val";;
            --host=*) host="$val";;
          esac
          args="$args $arg"
        fi
        ;;
    esac
  done
}

# Try to find a specific file within --basedir which can either be a binary
# release or installed source directory and return the path.
find_in_basedir()
{
  return_dir=0
  found=0
  case "$1" in
    --dir)
      return_dir=1; shift
      ;;
  esac

  file=$1; shift

  for dir in "$@"
  do
    if test -f "$basedir/$dir/$file"
    then
      found=1
      if test $return_dir -eq 1
      then
        echo "$basedir/$dir"
      else
        echo "$basedir/$dir/$file"
      fi
      break
    fi
  done

  if test $found -eq 0
  then
      # Test if command is in PATH
      $file --no-defaults --version > /dev/null 2>&1
      status=$?
      if test $status -eq 0
      then
        echo $file
      fi
  fi
}

cannot_find_file()
{
  echo
  echo "FATAL ERROR: Could not find $1"

  shift
  if test $# -ne 0
  then
    echo
    echo "The following directories were searched:"
    echo
    for dir in "$@"
    do
      echo "    $dir"
    done
  fi

  echo
  echo "If you compiled from source, you need to run 'make install' to"
  echo "copy the software into the correct location ready for operation."
  echo
  echo "If you are using a binary release, you must either be at the top"
  echo "level of the extracted archive, or pass the --basedir option"
  echo "pointing to that location."
  echo
}

# Ok, let's go.  We first need to parse arguments which are required by
# my_print_defaults so that we can execute it first, then later re-parse
# the command line to add any extra bits that we need.
parse_arguments PICK-ARGS-FROM-ARGV "$@"

#
# We can now find my_print_defaults.  This script supports:
#
#   --srcdir=path pointing to compiled source tree
#   --basedir=path pointing to installed binary location
#
# or default to compiled-in locations.
#

if test -n "$basedir"
then
  print_defaults=`find_in_basedir my_print_defaults bin extra`
  echo "print: $print_defaults"
  if test -z "$print_defaults"
  then
    cannot_find_file my_print_defaults $basedir/bin $basedir/extra
    exit 1
  fi
  mysql_command=`find_in_basedir mysql bin`
  if test -z "$mysql_command"
  then
    cannot_find_file mysql $basedir/bin
    exit 1
  fi
else
  print_defaults="@bindir@/my_print_defaults"
  mysql_command="@bindir@/mysql"
fi

if test ! -x "$print_defaults"
then
  cannot_find_file "$print_defaults"
  exit 1
fi

if test ! -x "$mysql_command"
then
  cannot_find_file "$mysql_command"
  exit 1
fi
# Now we can get arguments from the group [client], [client-server] and [client-mariadb]
# in the my.cfg file, then re-run to merge with command line arguments.
parse_arguments `$print_defaults $defaults_file $defaults_extra_file $no_defaults client client-server client-mariadb`
parse_arguments PICK-ARGS-FROM-ARGV "$@"

set_echo_compat() {
    case `echo "testing\c"`,`echo -n testing` in
	*c*,-n*) echo_n=   echo_c=     ;;
	*c*,*)   echo_n=-n echo_c=     ;;
	*)       echo_n=   echo_c='\c' ;;
    esac
}

validate_reply () {
    ret=0
    local default=${2:-y}
    if [ -z "$1" ]; then
	reply=$default
	return $ret
    fi
    case $1 in
        y|Y|yes|Yes|YES) reply=y ;;
        n|N|no|No|NO)    reply=n ;;
        *) ret=1 ;;
    esac
    return $ret
}

prepare() {
    touch $config $command
    chmod 600 $config $command
}

do_query() {
    if [ -n "$1" ]
    then
        echo "$1" >$command
        #sed 's,^,> ,' < $command  # Debugging
        $mysql_command --defaults-file=$config $defaults_extra_file $no_defaults --skip-column-names --batch $args <$command >$output
    else
        # rely on stdin
        $mysql_command --defaults-file=$config $defaults_extra_file $no_defaults --skip-column-names --batch $args >$output
    fi
    return $?
}

# Simple escape mechanism (\-escape any ' and \), suitable for two contexts:
# - single-quoted SQL strings
# - single-quoted option values on the right hand side of = in my.cnf
#
# These two contexts don't handle escapes identically.  SQL strings allow
# quoting any character (\C => C, for any C), but my.cnf parsing allows
# quoting only \, ' or ".  For example, password='a\b' quotes a 3-character
# string in my.cnf, but a 2-character string in SQL.
#
# This simple escape works correctly in both places.
basic_single_escape () {
    # The quoting on this sed command is a bit complex.  Single-quoted strings
    # don't allow *any* escape mechanism, so they cannot contain a single
    # quote.  The string sed gets (as argv[1]) is:  s/\(['\]\)/\\\1/g
    #
    # Inside a character class, \ and ' are not special, so the ['\] character
    # class is balanced and contains two characters.
    echo "$1" | sed 's/\(['"'"'\]\)/\\\1/g'
}

#
# create a simple my.cnf file to be able to pass the user password to the mysql
# client without putting it on the command line
#
make_config() {
    echo "# mysql_secure_installation config file" >$config
    echo "[mysql]" >>$config
    echo "user=$user" >>$config
    esc_pass=`basic_single_escape "$password"`
    echo "password='$esc_pass'" >>$config
    echo "${host:+host=$host}" >>$config
    #sed 's,^,> ,' < $config  # Debugging

    if test -n "$defaults_file"
    then
        dfile=`parse_arg "$defaults_file"`
        cat "$dfile" >>$config
    fi
}

get_user_and_password() {
    status_priv_user=1
    while [ $status_priv_user -ne 0 ]; do
    if test -z "$user"; then
        echo $echo_n "For which user do you want to specify a password (press enter for $USER): $echo_c"
        read user || interrupt
        echo
        if [ "x$user" = "x" ]; then
            emptyuser=1
            user=$USER
        else
            emptyuser=0
        fi
    fi
    if [ -z "$password" ] && [ "$emptyuser" -eq 0 ]; then
        stty -echo 2>/dev/null
        # If the empty user it means we are connecting with unix_socket else need password
        echo $echo_n "Enter current password for user $user (enter for none): $echo_c"
        read password || interrupt
        echo
        stty echo
    fi
    make_config
    # Only privileged user that has access to mysql DB can make changes
    do_query "use mysql"
    status_priv_user=$?
    if test $status_priv_user -ne 0; then
        echo "Only privileged user can make changes to mysql DB."
        if test $set_from_cli -eq 1; then
            clean_and_exit
        fi
        user=
        password=
    fi
    done
    do_query "show create user"
    if grep -q unix_socket "$output"; then
        unix_socket_auth=1
    else
        unix_socket_auth=0
    fi
    if grep -q "USING '" "$output"; then
        password_set=1
    else
        password_set=0
    fi
    read -r show_create < "$output" || interrupt
    echo "OK, successfully used password, moving on..."
    echo
}

set_user_password() {
    stty -echo 2>/dev/null
    echo $echo_n "New password: $echo_c"
    read password1 || interrupt
    echo
    echo $echo_n "Re-enter new password: $echo_c"
    read password || interrupt
    echo
    stty echo

    if [ "$password1" != "$password" ]; then
        echo "Sorry, passwords do not match."
        echo
        return 1
    fi

    if [ "$password1" = "" ]; then
        echo "Sorry, you can't use an empty password here."
        echo
        return 1
    fi
    esc_pass=$(basic_single_escape "$password1")
    do_query "SET PASSWORD = PASSWORD('$esc_pass')"
    if [ $? -eq 0 ]; then
        echo "Password updated successfully!"
    else
        echo "Password update failed!"
        clean_and_exit
    fi
    args="$args --password=$password"
    make_config

    return 0
}

remove_anonymous_users() {
    do_query <<EOANON
DROP USER /*M!100103 IF EXISTS */ ''@localhost;
/*M!100203 EXECUTE IMMEDIATE CONCAT('DROP USER IF EXISTS \'\'@', @@hostname) */;
EOANON
    if [ $? -eq 0 ]; then
        echo " ... Success!"
    else
        echo " ... Failed to remove anonymous users!"
        clean_and_exit
    fi

    return 0
}

remove_remote_root() {
    do_query <<-EOREMOTEROOT
DELIMITER &&
CREATE OR REPLACE PROCEDURE mysql.secure_users()
BEGIN
SELECT GROUP_CONCAT(DISTINCT CONCAT(QUOTE(user),'@',QUOTE(host))) INTO @users FROM mysql.global_priv WHERE user='root' AND host!='localhost';
IF @users IS NOT NULL THEN
	EXECUTE IMMEDIATE CONCAT('DROP USER ', @users);
END IF;
END;
&&
DELIMITER ;
/*M!100301 call mysql.secure_users() */;
/*M!100301 DROP PROCEDURE mysql.secure_users */;
EOREMOTEROOT

    if [ $? -eq 0 ]; then
        echo " ... Success!"
    else
        echo " ... Failed to remove remote root!"
    fi
}

check_test_database() {
    echo " - Checking the test databases..."
    do_query << EOCHECKTESTDB
SELECT schema_name FROM information_schema.schemata
WHERE  schema_name LIKE 'test';
EOCHECKTESTDB
    if [ $? -eq 0 ]; then
        if grep -q "test" "$output"; then
            return 1
        else
            return 0
        fi
    else
        echo " ... Failed to check test database!  Not critical, keep moving..."
    fi
}

remove_test_database() {
    echo " - Dropping test database..."
    do_query <<-EODROPTESTDB
DROP DATABASE IF EXISTS test;
EODROPTESTDB
    if [ $? -eq 0 ]; then
        echo " ... Success!"
    else
        echo " ... Failed to remove test database!  Not critical, keep moving..."
    fi

    echo " - Removing privileges on test database..."
    do_query <<-EOTEST
DELIMITER &&
CREATE OR REPLACE PROCEDURE mysql.secure_test_users()
BEGIN
SELECT GROUP_CONCAT(DISTINCT CONCAT(QUOTE(user),'@',QUOTE(host))) INTO @users FROM mysql.db JOIN mysql.global_priv USING (User,Host) WHERE Db='test';
IF @users IS NOT NULL THEN
	EXECUTE IMMEDIATE CONCAT('REVOKE ALL ON test.* FROM ', @users);
END IF;
SELECT GROUP_CONCAT(DISTINCT CONCAT(QUOTE(user),'@',QUOTE(host))) INTO @users FROM mysql.db JOIN mysql.global_priv USING (User,Host) WHERE Db='test\\_%';
IF @users IS NOT NULL THEN
	EXECUTE IMMEDIATE CONCAT('REVOKE ALL ON \`test\\_%\`.* FROM ', @users);
END IF;
DELETE FROM mysql.db WHERE User='' AND Db IN ('test', 'test\\_%');
IF ROW_COUNT() THEN
	FLUSH PRIVILEGES;
END IF;
END;
&&
DELIMITER ;
/*M!100301 call mysql.secure_test_users() */;
/*M!100301 DROP PROCEDURE mysql.secure_test_users */;
EOTEST

    if [ $? -eq 0 ]; then
        echo " ... Success!"
    else
        echo " ... Failed to remove privileges on test database!  Not critical, keep moving..."
    fi

    return 0
}

interrupt() {
    echo
    echo "Aborting!"
    echo
    cleanup
    stty echo
    exit 1
}

cleanup() {
    echo "Cleaning up..."
    rm -f $config $command $output
}

# Remove the files before exiting.
clean_and_exit() {
    cleanup
    exit 1
}

# The actual script starts here

prepare
set_echo_compat

echo
echo "NOTE: RUNNING ALL PARTS OF THIS SCRIPT IS RECOMMENDED FOR ALL MariaDB"
echo "      SERVERS IN PRODUCTION USE!  PLEASE READ EACH STEP CAREFULLY!"
echo
echo "In order to log into MariaDB to secure it, we'll need the current"
echo "password for a privileged user. If you've just installed MariaDB, and"
echo "haven't set a privileged password yet, you should just press enter here."
echo

get_user_and_password

if [ $user = root ] && [ $unix_socket_auth -ne 1 ]; then
    echo "Changing the root username obfuscates administrative users and"
    echo "helps prevent targeted attacks."
    echo
    echo $echo_n "Change root username to what username? (blank for no change) $echo_c"
    read reply || interrupt
    if [ -n "$reply" ]; then
    # Check user has @ in the name
    case "$reply" in
      *@*)
        user=${reply%@*}
        host=${reply#*@}
      ;;
      *)
        user=${reply}
        host="localhost"
      ;;
    esac

        do_query "EXECUTE IMMEDIATE CONCAT('RENAME USER ', CURRENT_USER(), ' TO \'$user\'@\'$host\'')"
        args="$args --user=$user --host=$host"
        make_config
    fi
fi

#
# Set unix_socket auth (if not already)
#

if [ $emptyuser -eq 0 ] && [ $unix_socket_auth -ne 1 ] && [ -z "$host" ] && [ "$host" = localhost ]; then
    echo "Setting the user to use unix_socket ensures that nobody"
    echo "can log into the MariaDB privileged user without being the same unix user."
    echo

    while true ; do
        echo $echo_n "Enable unix_socket authentication? [Y/n] $echo_c"
        read reply || interrupt
        validate_reply $reply && break
    done

    if [ "$reply" = "n" ]; then
        echo " ... skipping."
    else
        do_query "ALTER ${show_create:7} OR unix_socket"
        if [ $? -eq 0 ]; then
            echo "Enabled successfully!"
        else
            echo "Failed alter user!"
            clean_and_exit
        fi
    fi

fi
echo

#
# Set the user password
#

while true ; do
    if [ $unix_socket_auth -ne 1 ] || [ $password_set -ne 1 ]; then
        echo $echo_n "Set user: $user password? [Y/n] $echo_c"
        defsetpass=Y
    else
        echo "You already have your user account protected (unix_socket auth and password set, or password impossible to use), so you can safely answer 'n'."
        echo
        echo $echo_n "Set the user: $user password? [Y/n] $echo_c"
	defsetpass=N
    fi
    read reply || interrupt
    validate_reply $reply $defsetpass && break
done

if [ "$reply" = "n" ]; then
    echo " ... skipping."
else
    status=1
    while [ $status -eq 1 ]; do
        set_user_password
        status=$?
    done
fi
echo


#
# Remove anonymous users
#

echo "By default, a MariaDB installation has an anonymous user, allowing anyone"
echo "to log into MariaDB without having to have a user account created for"
echo "them.  This is intended only for testing, and to make the installation"
echo "go a bit smoother.  You should remove them before moving into a"
echo "production environment."
echo

while true ; do
    echo $echo_n "Remove anonymous users? [Y/n] $echo_c"
    read reply || interrupt
    validate_reply $reply && break
done
if [ "$reply" = "n" ]; then
    echo " ... skipping."
else
    remove_anonymous_users
fi
echo


#
# Remove test database
#

echo "By default, MariaDB comes with a database named 'test' that anyone can"
echo "access.  This is also intended only for testing, and should be removed"
echo "before moving into a production environment."
echo

while true ; do
    test_db_exists=0
    check_test_database
    if [ $? -eq 1 ]; then
        test_db_exists=1
        echo $echo_n "Remove test database and access to it? [Y/n] $echo_c"
        read reply || interrupt
        validate_reply $reply && break
    fi
    printf " ... Success!\nTest database doesn't exist!"
    break
done

if [ "$reply" = "n" ]; then
    echo " ... skipping."
else
    if [ $test_db_exists -eq 1 ]; then
        remove_test_database
    fi
fi
echo


#
# Disallow remote root login
#

echo "Normally, root should only be allowed to connect from 'localhost'.  This"
echo "ensures that someone cannot guess at the root password from the network."
echo
while true ; do
    echo $echo_n "Disallow root login remotely? [Y/n] $echo_c"
    read reply || interrupt
    validate_reply $reply && break
done
if [ "$reply" = "n" ]; then
    echo " ... skipping."
else
    remove_remote_root
fi
echo

cleanup

echo
echo "All done!  If you've completed all of the above steps, your MariaDB"
echo "installation should now be secure."
echo
echo "Thanks for using MariaDB!"
