#!/bin/sh
# Copyright (c) 2000, 2013, Oracle and/or its affiliates.
# Copyright (c) 2009, 2013, Monty Program Ab
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA

# This scripts creates the MariaDB Server system tables
#
# All unrecognized arguments to this script are passed to mysqld.

basedir=""
builddir=""
ldata="@localstatedir@"
langdir=""
srcdir=""

args=""
defaults=""
defaults_group_suffix=""
mysqld_opt=""
user=""
group=""
silent_startup="--silent-startup"

force=0
in_rpm=0
ip_only=0
cross_bootstrap=0
install_params="create database if not exists mysql;
create database if not exists test;
use mysql;"
auth_root_authentication_method=normal
auth_root_socket_user='root'

dirname0=`dirname $0 2>/dev/null`
dirname0=`dirname $dirname0 2>/dev/null`

usage()
{
  cat <<EOF
Usage: $0 [OPTIONS]
  --auth-root-authentication-method=normal|socket
                       Chooses the authentication method for the created initial
                       root user. The default is 'normal' to creates a root user
                       that can login without password, which can be insecure.
                       The alternative 'socket' allows only the system root user
                       to login as MariaDB root; this requires the unix socket
                       authentication plugin.
  --auth-root-socket-user=user
                       Used with --auth-root-authentication-method=socket. It
                       specifies the name of the MariaDB root account, as well
                       as of the system account allowed to access it. Defaults
                       to 'root'.
  --basedir=path       The path to the MariaDB installation directory.
  --builddir=path      If using --srcdir with out-of-directory builds, you
                       will need to set this to the location of the build
                       directory where built files reside.
  --cross-bootstrap    For internal use.  Used when building the MariaDB system
                       tables on a different host than the target.
  --datadir=path       The path to the MariaDB data directory.
  --no-defaults        Don't read default options from any option file.
  --defaults-extra-file=name
                       Read this file after the global files are read.
  --defaults-file=name Only read default options from the given file name.
  --defaults-group-suffix=name
                       In addition to the given groups, read also groups with
                       this suffix
  --force              Causes mysql_install_db to run even if DNS does not
                       work.  In that case, grant table entries that
                       normally use hostnames will use IP addresses.
  --help               Display this help and exit.                     
  --ldata=path         The path to the MariaDB data directory. Same as
                       --datadir.
  --rpm                For internal use.  This option is used by RPM files
                       during the MariaDB installation process.
  --skip-auth-anonymous-user
                       Do not install an unprivileged anonymous user.
  --skip-name-resolve  Use IP addresses rather than hostnames when creating
                       grant table entries.  This option can be useful if
                       your DNS does not work.
  --srcdir=path        The path to the MariaDB source directory.  This option
                       uses the compiled binaries and support files within the
                       source tree, useful for if you don't want to install
                       MariaDB yet and just want to create the system tables.
  --user=user_name     The login username to use for running mysqld.  Files
                       and directories created by mysqld will be owned by this
                       user.  You must be root to use this option.  By default
                       mysqld runs using your current login name and files and
                       directories that it creates will be owned by you.
  --group=group_name   The login group to use for running mysqld.  Files and
                       directories created by mysqld will be owned by this
                       group. You must be root to use this option.  By default
                       mysqld runs using your current group and files and
                       directories that it creates will be owned by you.

All other options are passed to the mysqld program

EOF
  exit 1
}

s_echo()
{
  if test "$in_rpm" -eq 0 -a "$cross_bootstrap" -eq 0
  then
    echo "$1"
  fi
}

link_to_help()
{
  echo
  echo "The latest information about mysql_install_db is available at"
  echo "https://mariadb.com/kb/en/installing-system-tables-mysql_install_db"
}

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
    case "$arg" in
      --force) force=1 ;;
      --basedir=*) basedir=`parse_arg "$arg"` ;;
      --builddir=*) builddir=`parse_arg "$arg"` ;;
      --srcdir=*)  srcdir=`parse_arg "$arg"` ;;
      --ldata=*|--datadir=*|--data=*) ldata=`parse_arg "$arg"` ;;
        # Note that the user will be passed to mysqld so that it runs
        # as 'user' (crucial e.g. if log-bin=/some_other_path/
        # where a chown of datadir won't help)
      --user=*) user=`parse_arg "$arg"` ;;
      --group=*) group=`parse_arg "$arg"` ;;
      --skip-name-resolve) ip_only=1 ;;
      --verbose) verbose=1 ; silent_startup="" ;;
      --rpm) in_rpm=1 ;;
      --help) usage ;;
      --no-defaults|--defaults-file=*|--defaults-extra-file=*)
        defaults="$arg" ;;
      --defaults-group-suffix=*)
        defaults_group_suffix="$arg" ;;

      --cross-bootstrap|--windows)
        # Used when building the MariaDB system tables on a different host than
        # the target. The platform-independent files that are created in
        # --datadir on the host can be copied to the target system.
        #
        # The most common use for this feature is in the Windows installer
        # which will take the files from datadir and include them as part of
        # the install package.  See top-level 'dist-hook' make target.
        #
        # --windows is a deprecated alias
        cross_bootstrap=1 ;;
      --skip-auth-anonymous-user)
	install_params="$install_params
SET @skip_auth_anonymous=1;" ;;
      --auth-root-authentication-method=normal)
	auth_root_authentication_method=normal ;;
      --auth-root-authentication-method=socket)
	auth_root_authentication_method=socket ;;
      --auth-root-authentication-method=*)
        usage ;;
      --auth-root-socket-user=*)
        auth_root_socket_user="$(parse_arg "$arg")" ;;

      *)
        if test -n "$pick_args"
        then
          # This sed command makes sure that any special chars are quoted,
          # so the arg gets passed exactly to the server.
          # XXX: This is broken; true fix requires using eval and proper
          # quoting of every single arg ($basedir, $ldata, etc.)
          #args="$args "`echo "$arg" | sed -e 's,\([^a-zA-Z0-9_.-]\),\\\\\1,g'`
          args="$args $arg"
        fi
        ;;
    esac
  done
}

# Try to find a specific file within --basedir which can either be a binary
# release or installed source directory and return the path.
find_in_dirs()
{
  case "$1" in
    --dir)
      return_dir=1; shift
      ;;
  esac

  file=$1; shift

  for dir in "$@"
  do
    if test -f "$dir/$file"
    then
      if test -n "$return_dir"
      then
        echo "$dir"
      else
        echo "$dir/$file"
      fi
      break
    fi
  done
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
  echo "If you compiled from source, you need to either run 'make install' to"
  echo "copy the software into the correct location ready for operation."
  echo "If you don't want to do a full install, you can use the --srcdir"
  echo "option to only install the mysql database and privilege tables."
  echo
  echo "If you are using a binary release, you must either be at the top"
  echo "level of the extracted archive, or pass the --basedir option"
  echo "pointing to that location."
  link_to_help
}

# Ok, let's go.  We first need to parse arguments which are required by
# my_print_defaults so that we can execute it first, then later re-parse
# the command line to add any extra bits that we need.
parse_arguments "$@"

#
# We can now find my_print_defaults.  This script supports:
#
#   --srcdir=path pointing to compiled source tree
#   --basedir=path pointing to installed binary location
#
# or default to compiled-in locations.
#
if test -n "$srcdir" && test -n "$basedir"
then
  echo "ERROR: Specify either --basedir or --srcdir, not both."
  link_to_help
  exit 1
fi
if test -n "$srcdir"
then
  # In an out-of-source build, builddir is not srcdir. Try to guess where
  # builddir is by looking for my_print_defaults.
  if test -z "$builddir"
  then
    if test -x "$dirname0/extra/my_print_defaults"
    then
      builddir="$dirname0"
    else
      builddir="$srcdir"
    fi
  fi
  print_defaults="$builddir/extra/my_print_defaults"
elif test -n "$basedir"
then
  print_defaults=`find_in_dirs my_print_defaults $basedir/bin $basedir/extra`
  if test -z "$print_defaults"
  then
    cannot_find_file my_print_defaults $basedir/bin $basedir/extra
    exit 1
  fi
elif test -n "$dirname0" -a -x "$dirname0/@bindir@/my_print_defaults"
then
  print_defaults="$dirname0/@bindir@/my_print_defaults"
else
  print_defaults="@bindir@/my_print_defaults"
fi

if test ! -x "$print_defaults"
then
  cannot_find_file "$print_defaults"
  exit 1
fi

# Now we can get arguments from the groups [mysqld] and [mysql_install_db]
# in the my.cfg file, then re-run to merge with command line arguments.
parse_arguments `"$print_defaults" $defaults $defaults_group_suffix --mysqld mysql_install_db`
parse_arguments PICK-ARGS-FROM-ARGV "$@"

rel_mysqld="$dirname0/@INSTALL_SBINDIR@/mysqld"

# Configure paths to support files
if test -n "$srcdir"
then
  basedir="$builddir"
  bindir="$basedir/client"
  resolveip="$basedir/extra/resolveip"
  mysqld="$basedir/sql/mysqld"
  langdir="$basedir/sql/share/english"
  srcpkgdatadir="$srcdir/scripts"
  buildpkgdatadir="$builddir/scripts"
  plugindir="$builddir/plugin/auth_socket"
elif test -n "$basedir"
then
  bindir="$basedir/bin" # only used in the help text
  resolveip=`find_in_dirs resolveip @resolveip_locations@`
  if test -z "$resolveip"
  then
    cannot_find_file resolveip @resolveip_locations@
    exit 1
  fi
  mysqld=`find_in_dirs mysqld @mysqld_locations@`
  if test -z "$mysqld"
  then
    cannot_find_file mysqld @mysqld_locations@
    exit 1
  fi
  langdir=`find_in_dirs --dir errmsg.sys @errmsg_locations@`
  if test -z "$langdir"
  then
    cannot_find_file errmsg.sys @errmsg_locations@
    exit 1
  fi
  srcpkgdatadir=`find_in_dirs --dir fill_help_tables.sql @pkgdata_locations@`
  buildpkgdatadir=$srcpkgdatadir
  if test -z "$srcpkgdatadir"
  then
    cannot_find_file fill_help_tables.sql @pkgdata_locations@
    exit 1
  fi
  plugindir=`find_in_dirs --dir auth_socket.so $basedir/lib*/plugin $basedir/lib*/mysql/plugin`
# relative from where the script was run for a relocatable install
elif test -n "$dirname0" -a -x "$rel_mysqld" -a ! "$rel_mysqld" -ef "@sbindir@/mysqld"
then
  basedir="$dirname0"
  bindir="$basedir/@INSTALL_BINDIR@"
  resolveip="$bindir/resolveip"
  mysqld="$rel_mysqld"
  srcpkgdatadir="$basedir/@INSTALL_MYSQLSHAREDIR@"
  buildpkgdatadir="$basedir/@INSTALL_MYSQLSHAREDIR@"
  plugindir="$basedir/@INSTALL_PLUGINDIR@"
else
  basedir="@prefix@"
  bindir="@bindir@"
  resolveip="$bindir/resolveip"
  mysqld="@sbindir@/mysqld"
  srcpkgdatadir="@pkgdatadir@"
  buildpkgdatadir="@pkgdatadir@"
  plugindir="@pkgplugindir@"
fi

# Set up paths to SQL scripts required for bootstrap
fill_help_tables="$srcpkgdatadir/fill_help_tables.sql"
create_system_tables="$srcpkgdatadir/mysql_system_tables.sql"
create_system_tables2="$srcpkgdatadir/mysql_performance_tables.sql"
fill_system_tables="$srcpkgdatadir/mysql_system_tables_data.sql"
maria_add_gis_sp="$buildpkgdatadir/maria_add_gis_sp_bootstrap.sql"

for f in "$fill_help_tables" "$create_system_tables" "$create_system_tables2" "$fill_system_tables" "$maria_add_gis_sp"
do
  if test ! -f "$f"
  then
    cannot_find_file "$f"
    exit 1
  fi
done

if test ! -x "$mysqld"
then
  cannot_find_file "$mysqld"
  exit 1
fi

if test -n "$langdir"
then
  if test ! -f "$langdir/errmsg.sys"
  then
    cannot_find_file "$langdir/errmsg.sys"
    exit 1
  fi
  mysqld_opt="--lc-messages-dir=$langdir/.."
else
  mysqld_opt="--lc-messages=en_US"
fi


# Try to determine the hostname
hostname=`@HOSTNAME@`

# Check if hostname is valid
if test "$cross_bootstrap" -eq 0 -a "$in_rpm" -eq 0 -a "$force" -eq 0
then
  resolved=`"$resolveip" $hostname 2>&1`
  if test $? -ne 0
  then
    resolved=`"$resolveip" localhost 2>&1`
    if test $? -ne 0
    then
      echo "Neither host '$hostname' nor 'localhost' could be looked up with"
      echo "'$resolveip'"
      echo "Please configure the 'hostname' command to return a correct"
      echo "hostname."
      echo "If you want to solve this at a later stage, restart this script"
      echo "with the --force option"
      link_to_help
      exit 1
    fi
    echo "WARNING: The host '$hostname' could not be looked up with $resolveip."
    echo "This probably means that your libc libraries are not 100 % compatible"
    echo "with this binary MariaDB version. The MariaDB daemon, mysqld, should work"
    echo "normally with the exception that host name resolving will not work."
    echo "This means that you should use IP addresses instead of hostnames"
    echo "when specifying MariaDB privileges !"
  fi
fi

if test "$ip_only" -eq 1
then
  hostname=`echo "$resolved" | awk '/ /{print $6}'`
fi

# Create database directories
for dir in "$ldata"
do
  if test ! -d "$dir"
  then
    if ! `mkdir -p "$dir"`
    then
      echo "Fatal error Can't create database directory '$dir'"
      link_to_help
      exit 1
    fi
    chmod 700 "$dir"
  fi
  if test -n "$user"
  then
    if test -z "$group"
    then
      chown $user $dir
    else
      chown $user:$group $dir
    fi
    if test $? -ne 0
    then
      echo "Cannot change ownership of the database directories to the '$user'"
      echo "user.  Check that you have the necessary permissions and try again."
      exit 1
    fi
  fi
done

if test -n "$user"
then
  args="$args --user=$user"
fi

#To be enabled if/when we enable --group as an option to mysqld
#if test -n "$group"
#then
#  args="$args --group=$group"
#fi

# When doing a "cross bootstrap" install, no reference to the current
# host should be added to the system tables.  So we filter out any
# lines which contain the current host name.
if test $cross_bootstrap -eq 1
then
  filter_cmd_line="sed -e '/@current_hostname/d'"
else
  filter_cmd_line="cat"
fi

# Configure mysqld command line
mysqld_bootstrap="${MYSQLD_BOOTSTRAP-$mysqld}"
mysqld_install_cmd_line()
{
  "$mysqld_bootstrap" $defaults $defaults_group_suffix "$mysqld_opt" --bootstrap $silent_startup\
  "--basedir=$basedir" "--datadir=$ldata" --log-warnings=0 --enforce-storage-engine="" \
  "--plugin-dir=${plugindir}" \
  $args --max_allowed_packet=8M \
  --net_buffer_length=16K
}


# Create the system and help tables by passing them to "mysqld --bootstrap"
s_echo "Installing MariaDB/MySQL system tables in '$ldata' ..."
case "$auth_root_authentication_method" in
  normal)
    install_params="$install_params
SET @skip_auth_root_nopasswd=NULL;
SET @auth_root_socket=NULL;" ;;
  socket)
    install_params="$install_params
SET @skip_auth_root_nopasswd=1;
SET @auth_root_socket='$auth_root_socket_user';" ;;
esac
if { echo "$install_params"; cat "$create_system_tables" "$create_system_tables2" "$fill_system_tables" "$fill_help_tables" "$maria_add_gis_sp"; } | eval "$filter_cmd_line" | mysqld_install_cmd_line > /dev/null
then
    printf "@VERSION@-MariaDB" > "$ldata/mysql_upgrade_info"
  s_echo "OK"
else
  echo
  echo "Installation of system tables failed!  Examine the logs in"
  echo "$ldata for more information."
  echo
  echo "The problem could be conflicting information in an external"
  echo "my.cnf files. You can ignore these by doing:"
  echo
  echo "    shell> $0 --defaults-file=~/.my.cnf"
  echo
  echo "You can also try to start the mysqld daemon with:"
  echo
  echo "    shell> $mysqld --skip-grant-tables --general-log &"
  echo
  echo "and use the command line tool $bindir/mysql"
  echo "to connect to the mysql database and look at the grant tables:"
  echo
  echo "    shell> $bindir/mysql -u root mysql"
  echo "    mysql> show tables;"
  echo
  echo "Try 'mysqld --help' if you have problems with paths.  Using"
  echo "--general-log gives you a log in $ldata that may be helpful."
  link_to_help
  echo "You can find the latest source at https://downloads.mariadb.org and"
  echo "the maria-discuss email list at https://launchpad.net/~maria-discuss"
  echo
  echo "Please check all of the above before submitting a bug report"
  echo "at http://mariadb.org/jira"
  echo
  exit 1
fi

# Don't output verbose information if running inside bootstrap or using
# --srcdir for testing.  In such cases, there's no end user looking at
# the screen.
if test "$cross_bootstrap" -eq 0 && test -z "$srcdir"
then
  s_echo
  s_echo "To start mysqld at boot time you have to copy"
  s_echo "support-files/mysql.server to the right place for your system"

  if test "$auth_root_authentication_method" = normal
  then
    echo
    echo
    echo "PLEASE REMEMBER TO SET A PASSWORD FOR THE MariaDB root USER !"
    echo "To do so, start the server, then issue the following command:"
    echo
    echo "'$bindir/mysql_secure_installation'"
    echo
    echo "which will also give you the option of removing the test"
    echo "databases and anonymous user created by default.  This is"
    echo "strongly recommended for production servers."
  fi

  echo
  echo "See the MariaDB Knowledgebase at http://mariadb.com/kb"

  if test "$in_rpm" -eq 0
  then
    echo
    echo "You can start the MariaDB daemon with:"
    echo "cd '$basedir' ; $bindir/mysqld_safe --datadir='$ldata'"
    echo
    echo "You can test the MariaDB daemon with mysql-test-run.pl"
    echo "cd '$basedir/mysql-test' ; perl mysql-test-run.pl"
  fi

  echo
  echo "Please report any problems at http://mariadb.org/jira"
  echo
  echo "The latest information about MariaDB is available at http://mariadb.org/."
  echo
  echo "Consider joining MariaDB's strong and vibrant community:"
  echo "https://mariadb.org/get-involved/"
  echo
fi

exit 0
