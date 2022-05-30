#!/bin/sh
# Copyright Abandoned 1996 TCX DataKonsult AB & Monty Program KB & Detron HB
# This file is public domain and comes with NO WARRANTY of any kind
#
# Script to start the MySQL daemon and restart it if it dies unexpectedly
#
# This should be executed in the MySQL base directory if you are using a
# binary installation that is not installed in its compile-time default
# location
#
# mysql.server works by first doing a cd to the base directory and from there
# executing mysqld_safe

# Initialize script globals
KILL_MYSQLD=1;
MYSQLD=
niceness=0
nowatch=0
mysqld_ld_preload=
mysqld_ld_library_path=
flush_caches=0
numa_interleave=0
wsrep_on=0
dry_run=0
defaults_group_suffix=

# Initial logging status: error log is not open, and not using syslog
logging=init
want_syslog=0
syslog_tag=
user='@MYSQLD_USER@'
group='@MYSQLD_GROUP@'
pid_file=
err_log=
err_log_base=
skip_err_log=0

syslog_tag_mysqld=mysqld
syslog_tag_mysqld_safe=mysqld_safe

trap '' 1 2 3 15			# we shouldn't let anyone kill us

# MySQL-specific environment variable. First off, it's not really a umask,
# it's the desired mode. Second, it follows umask(2), not umask(3) in that
# octal needs to be explicit. Our shell might be a proper sh without printf,
# multiple-base arithmetic, and binary arithmetic, so this will get ugly.
# We reject decimal values to keep things at least half-sane.
umask 007                               # fallback
UMASK="${UMASK-0640}"
fmode=`echo "$UMASK" | sed -e 's/[^0246]//g'`
octalp=`echo "$fmode"|cut -c1`
fmlen=`echo "$fmode"|wc -c|sed -e 's/ //g'`
if [ "x$octalp" != "x0" -o "x$UMASK" != "x$fmode" -o "x$fmlen" != "x5" ]
then
  fmode=0640
  echo "UMASK must be a 3-digit mode with an additional leading 0 to indicate octal." >&2
  echo "The first digit will be corrected to 6, the others may be 0, 2, 4, or 6." >&2
fi
fmode=`echo "$fmode"|cut -c3-4`
fmode="6$fmode"
if [ "x$UMASK" != "x0$fmode" ]
then
  echo "UMASK corrected from $UMASK to 0$fmode ..."
fi

defaults=
case "$1" in
    --no-defaults|--defaults-file=*|--defaults-extra-file=*)
      defaults="$1"; shift
      ;;
esac

usage () {
        cat <<EOF
Usage: $0 [OPTIONS]
  --no-defaults              Don't read the system defaults file
  --defaults-file=FILE       Use the specified defaults file
  --defaults-extra-file=FILE Also use defaults from the specified file
  --defaults-group-suffix=X  Additionally read default groups with X appended
                             as a suffix
  --ledir=DIRECTORY          Look for mysqld in the specified directory
  --open-files-limit=LIMIT   Limit the number of open files
  --crash-script=FILE        Script to call when mysqld crashes
  --core-file-size=LIMIT     Limit core files to the specified size
  --timezone=TZ              Set the system timezone
  --malloc-lib=LIB           Preload shared library LIB if available
  --mysqld=FILE              Use the specified file as mysqld
  --mysqld-version=VERSION   Use "mysqld-VERSION" as mysqld
  --dry-run                  Simulate the start to detect errors but don't start
  --nice=NICE                Set the scheduling priority of mysqld
  --no-auto-restart          Exit after starting mysqld
  --nowatch                  Exit after starting mysqld
  --plugin-dir=DIR           Plugins are under DIR or DIR/VERSION, if
                             VERSION is given
  --skip-kill-mysqld         Don't try to kill stray mysqld processes
  --syslog                   Log messages to syslog with 'logger'
  --skip-syslog              Log messages to error log (default)
  --syslog-tag=TAG           Pass -t "mysqld-TAG" to 'logger'
  --flush-caches             Flush and purge buffers/caches before
                             starting the server
  --numa-interleave          Run mysqld with its memory interleaved
                             on all NUMA nodes

All other options are passed to the mysqld program.

EOF
        exit 1
}

find_in_bin() {
  if test -x "$MY_BASEDIR_VERSION/bin/$1"
  then
    echo "$MY_BASEDIR_VERSION/bin/$1"
  elif test -x "@bindir@/$1"
  then
    echo "@bindir@/$1"
  else
    echo "$1"
  fi
}

log_generic () {
  [ $dry_run -eq 1 ] && return
  priority="$1"
  shift

  msg="`date +'%y%m%d %H:%M:%S'` mysqld_safe $*"
  echo "$msg"
  case $logging in
    init) ;;  # Just echo the message, don't save it anywhere
    file)
      if [ "$helper_exist" -eq "0" ]; then
        echo "$msg" | "$helper" "$user" log "$err_log"
      fi
    ;;
    syslog) logger -t "$syslog_tag_mysqld_safe" -p "$priority" "$*" ;;
    *)
      echo "Internal program error (non-fatal):" \
           " unknown logging method '$logging'" >&2
      ;;
  esac
}

log_error () {
  log_generic daemon.error "$@" >&2
}

log_notice () {
  log_generic daemon.notice "$@"
}

eval_log_error () {
  local cmd="$1"
  case $logging in
    file)
     if [ "$helper_exist" -eq "0" ]; then
        cmd="$cmd 2>&1 | "`shell_quote_string "$helper"`" $user log "`shell_quote_string "$err_log"`
     fi
     ;;
    syslog)
      # mysqld often prefixes its messages with a timestamp, which is
      # redundant when logging to syslog (which adds its own timestamp)
      # However, we don't strip the timestamp with sed here, because
      # sed buffers output (only GNU sed supports a -u (unbuffered) option)
      # which means that messages may not get sent to syslog until the
      # mysqld process quits.
      cmd="$cmd 2>&1 | logger -t '$syslog_tag_mysqld' -p daemon.error"
      ;;
    *)
      echo "Internal program error (non-fatal):" \
           " unknown logging method '$logging'" >&2
      ;;
  esac

  if test $nowatch -eq 1
  then
    # We'd prefer to exec $cmd here, but SELinux needs to be fixed first
    #/usr/bin/logger "Running mysqld: $cmd"
    eval "$cmd &"
    exit 0
  else
    #echo "Running mysqld: [$cmd]"
    eval "$cmd"
  fi
}

shell_quote_string() {
  # This sed command makes sure that any special chars are quoted,
  # so the arg gets passed exactly to the server.
  echo "$1" | sed -e 's,\([^a-zA-Z0-9/_.=-]\),\\\1,g'
}

wsrep_pick_url() {
  [ $# -eq 0 ] && return 0

  log_error "WSREP: 'wsrep_urls' is DEPRECATED! Use wsrep_cluster_address to specify multiple addresses instead."

  if ! command -v nc >/dev/null
  then
    log_error "ERROR: nc tool not found in PATH! Make sure you have it installed."
    return 1
  fi

  local url
  # Assuming URL in the form scheme://host:port
  # If host and port are not NULL, the liveness of URL is assumed to be tested
  # If port part is absent, the url is returned literally and unconditionally
  # If every URL has port but none is reachable, nothing is returned
  for url in `echo $@ | sed s/,/\ /g` 0; do
    local host=`echo $url | cut -d \: -f 2 | sed s/^\\\/\\\///`
    local port=`echo $url | cut -d \: -f 3`
    [ -z "$port" ] && break
    nc -z "$host" $port >/dev/null && break
  done

  if [ "$url" == "0" ]; then
    log_error "ERROR: none of the URLs in '$@' is reachable."
    return 1
  fi

  echo $url
}

# Run mysqld with --wsrep-recover and parse recovered position from log.
# Position will be stored in wsrep_start_position_opt global.
wsrep_start_position_opt=""
wsrep_recover_position() {
  local mysqld_cmd="$@"
  local euid=$(id -u)
  local ret=0

  local wr_logfile=$(mktemp /tmp/wsrep_recovery.XXXXXX)

  # safety checks
  if [ -z $wr_logfile ]; then
    log_error "WSREP: mktemp failed"
    return 1
  fi

  if [ -f $wr_logfile ]; then
    # NOTE! Do not change ownership of the temporary file, as on newer kernel
    # versions fs.protected_regular is set to '2' and redirecting output with >
    # as root to a file not owned by root will fail with "Permission denied"
    chmod 600 $wr_logfile
  else
    log_error "WSREP: mktemp failed"
    return 1
  fi

  local wr_pidfile="$DATADIR/"`@HOSTNAME@`"-recover.pid"

  local wr_options="--disable-log-error  --pid-file='$wr_pidfile'"

  log_notice "WSREP: Running position recovery with $wr_options"

  eval "$mysqld_cmd --wsrep_recover $wr_options 2> $wr_logfile"

  if [ ! -s "$wr_logfile" ]; then
    log_error "Log file $wr_logfile was empty, cannot proceed. Is system running fs.protected_regular?"
    exit 1
  fi

  local rp="$(grep 'WSREP: Recovered position:' $wr_logfile)"
  if [ -z "$rp" ]; then
    local skipped="$(grep WSREP $wr_logfile | grep 'skipping position recovery')"
    if [ -z "$skipped" ]; then
      log_error "WSREP: Failed to recover position: '`cat $wr_logfile`'"
      ret=1
    else
      log_notice "WSREP: Position recovery skipped"
    fi
  else
    local start_pos="$(echo $rp | sed 's/.*WSREP\:\ Recovered\ position://' \
        | sed 's/^[ \t]*//')"
    log_notice "WSREP: Recovered position $start_pos"
    wsrep_start_position_opt="--wsrep_start_position=$start_pos"
  fi

  if [ $ret -eq 0 ] ; then
    local wr_logfile_permanent="$DATADIR/wsrep_recovery.ok"
  else
    local wr_logfile_permanent="$DATADIR/wsrep_recovery.fail"
  fi
  touch $wr_logfile_permanent
  [ "$euid" = "0" ] && chown $user $wr_logfile_permanent
  chmod 600 $wr_logfile_permanent
  cat "$wr_logfile" >> $wr_logfile_permanent
  rm -f "$wr_logfile"

  return $ret
}

parse_arguments() {
  for arg do
    val=`echo "$arg" | sed -e "s;--[^=]*=;;"`
    case "$arg" in
      # these get passed explicitly to mysqld
      --basedir=*) MY_BASEDIR_VERSION="$val" ;;
      --datadir=*|--data=*) DATADIR="$val" ;;
      --pid[-_]file=*) pid_file="$val" ;;
      --plugin[-_]dir=*) PLUGIN_DIR="$val" ;;
      --user=*) user="$val"; SET_USER=1 ;;
      --group=*) group="$val"; SET_USER=1 ;;
      --log[-_]basename=*|--hostname=*|--loose[-_]log[-_]basename=*)
        pid_file="$val.pid";
	err_log_base="$val";
	;;

      # these might have been set in a [mysqld_safe] section of my.cnf
      # they are added to mysqld command line to override settings from my.cnf
      --skip[-_]log[-_]error)
        err_log=;
        skip_err_log=1;
        ;;
      --log[-_]error=*)
        err_log="$val";
        skip_err_log=0;
        ;;
      --port=*) mysql_tcp_port="$val" ;;
      --socket=*) mysql_unix_port="$val" ;;

      # mysqld_safe-specific options - must be set in my.cnf ([mysqld_safe])!
      --core[-_]file[-_]size=*) core_file_size="$val" ;;
      --ledir=*) ledir="$val" ;;
      --malloc[-_]lib=*) set_malloc_lib "$val" ;;
      --crash[-_]script=*) crash_script="$val" ;;
      --mysqld=*) MYSQLD="$val" ;;
      --mysqld[-_]version=*)
        if test -n "$val"
        then
          MYSQLD="mysqld-$val"
          PLUGIN_VARIANT="/$val"
        else
          MYSQLD="mysqld"
        fi
        ;;
      --dry[-_]run) dry_run=1 ;;
      --nice=*) niceness="$val" ;;
      --nowatch|--no[-_]watch|--no[-_]auto[-_]restart) nowatch=1 ;;
      --open[-_]files[-_]limit=*) open_files="$val" ;;
      --skip[-_]kill[-_]mysqld*) KILL_MYSQLD=0 ;;
      --syslog) want_syslog=1 ;;
      --skip[-_]syslog) want_syslog=0 ;;
      --syslog[-_]tag=*) syslog_tag="$val" ;;
      --timezone=*) TZ="$val"; export TZ; ;;
      --flush[-_]caches) flush_caches=1 ;;
      --numa[-_]interleave) numa_interleave=1 ;;
      --wsrep[-_]on)
        wsrep_on=1
        append_arg_to_args "$arg"
        ;;
      --skip[-_]wsrep[-_]on)
        wsrep_on=0
        append_arg_to_args "$arg"
        ;;
      --wsrep[-_]on=*)
        if echo $val | grep -iq '\(ON\|1\)'; then
          wsrep_on=1
        else
          wsrep_on=0
        fi
        append_arg_to_args "$arg"
        ;;
      --wsrep[-_]urls=*) wsrep_urls="$val"; ;;
      --wsrep[-_]provider=*)
        if test -n "$val" && test "$val" != "none"
        then
          wsrep_restart=1
        fi
        append_arg_to_args "$arg"
        ;;

      --defaults-group-suffix=*) defaults_group_suffix="$arg" ;;

      --help) usage ;;

      *)
        case "$unrecognized_handling" in
          collect) append_arg_to_args "$arg" ;;
          complain) log_error "unknown option '$arg'" ;;
        esac
    esac
  done
}


# Add a single shared library to the list of libraries which will be added to
# LD_PRELOAD for mysqld
#
# Since LD_PRELOAD is a space-separated value (for historical reasons), if a
# shared lib's path contains spaces, that path will be prepended to
# LD_LIBRARY_PATH and stripped from the lib value.
add_mysqld_ld_preload() {
  lib_to_add="$1"
  log_notice "Adding '$lib_to_add' to LD_PRELOAD for mysqld"

  case "$lib_to_add" in
    *' '*)
      # Must strip path from lib, and add it to LD_LIBRARY_PATH
      lib_file=`basename "$lib_to_add"`
      case "$lib_file" in
        *' '*)
          # The lib file itself has a space in its name, and can't
          # be used in LD_PRELOAD
          log_error "library name '$lib_to_add' contains spaces and can not be used with LD_PRELOAD"
          exit 1
          ;;
      esac
      lib_path=`dirname "$lib_to_add"`
      lib_to_add="$lib_file"
      [ -n "$mysqld_ld_library_path" ] && mysqld_ld_library_path="$mysqld_ld_library_path:"
      mysqld_ld_library_path="$mysqld_ld_library_path$lib_path"
      ;;
  esac

  # LD_PRELOAD is a space-separated
  [ -n "$mysqld_ld_preload" ] && mysqld_ld_preload="$mysqld_ld_preload "
  mysqld_ld_preload="${mysqld_ld_preload}$lib_to_add"
}


# Returns LD_PRELOAD (and LD_LIBRARY_PATH, if needed) text, quoted to be
# suitable for use in the eval that calls mysqld.
#
# All values in mysqld_ld_preload are prepended to LD_PRELOAD.
mysqld_ld_preload_text() {
  text=

  if [ -n "$mysqld_ld_preload" ]; then
    new_text="$mysqld_ld_preload"
    [ -n "$LD_PRELOAD" ] && new_text="$new_text $LD_PRELOAD"
    text="${text}LD_PRELOAD="`shell_quote_string "$new_text"`' '
  fi

  if [ -n "$mysqld_ld_library_path" ]; then
    new_text="$mysqld_ld_library_path"
    [ -n "$LD_LIBRARY_PATH" ] && new_text="$new_text:$LD_LIBRARY_PATH"
    text="${text}LD_LIBRARY_PATH="`shell_quote_string "$new_text"`' '
  fi

  echo "$text"
}

# set_malloc_lib LIB
# - If LIB is empty, do nothing and return
# - If LIB starts with 'tcmalloc' or 'jemalloc', look for the shared library
#   using `ldconfig`.
#   tcmalloc is part of the Google perftools project.
# - If LIB is an absolute path, assume it is a malloc shared library
#
# Put LIB in mysqld_ld_preload, which will be added to LD_PRELOAD when
# running mysqld.  See ld.so for details.
set_malloc_lib() {
  malloc_lib="$1"
  if expr "$malloc_lib" : "\(tcmalloc\|jemalloc\)" > /dev/null ; then
    export PATH=$PATH:/sbin
    if ! command -v ldconfig > /dev/null 2>&1
    then
      log_error "ldconfig command not found, required for ldconfig -p"
      exit 1
    fi
    # format from ldconfig:
    # "libjemalloc.so.1 (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/libjemalloc.so.1"
    libmalloc_path="$(ldconfig -p | sed -n "/lib${malloc_lib}/p" | cut -d '>' -f2)"

    if [ -z "$libmalloc_path" ]; then
      log_error "no shared library for lib$malloc_lib.so.[0-9] found."
      exit 1
    fi

    for f in $libmalloc_path; do
      if [ -f "$f" ]; then
        malloc_lib=$f # get the first path if many
        break
      fi
    done
  fi
  # Allow --malloc-lib='' to override other settings
  [ -z  "$malloc_lib" ] && return

  case "$malloc_lib" in
    /*)
      if [ ! -r "$malloc_lib" ]; then
        log_error "--malloc-lib '$malloc_lib' can not be read and will not be used"
        exit 1
      fi
      ;;
    *)
      log_error "--malloc-lib must be an absolute path, 'tcmalloc' or " \
      "'jemalloc'; ignoring value '$malloc_lib'"
      exit 1
      ;;
  esac
  add_mysqld_ld_preload "$malloc_lib"
}


#
# First, try to find BASEDIR and ledir (where mysqld is)
#

MY_PWD=`dirname $0`
MY_PWD=`cd "$MY_PWD"/.. && pwd`
# Check for the directories we would expect from a binary release install
if test -n "$MY_BASEDIR_VERSION" -a -d "$MY_BASEDIR_VERSION"
then
  # BASEDIR is already overridden on command line.  Do not re-set.

  # Use BASEDIR to discover le.
  if test -x "$MY_BASEDIR_VERSION/libexec/mariadbd"
  then
    ledir="$MY_BASEDIR_VERSION/libexec"
  elif test -x "$MY_BASEDIR_VERSION/sbin/mariadbd"
  then
    ledir="$MY_BASEDIR_VERSION/sbin"
  else
    ledir="$MY_BASEDIR_VERSION/bin"
  fi
elif test -x "$MY_PWD/bin/mariadbd"
then
  MY_BASEDIR_VERSION="$MY_PWD"		# Where bin, share and data are
  ledir="$MY_PWD/bin"			# Where mysqld is
# Check for the directories we would expect from a source install
elif test -x "$MY_PWD/libexec/mariadbd"
then
  MY_BASEDIR_VERSION="$MY_PWD"		# Where libexec, share and var are
  ledir="$MY_PWD/libexec"		# Where mysqld is
elif test -x "$MY_PWD/sbin/mariadbd"
then
  MY_BASEDIR_VERSION="$MY_PWD"		# Where sbin, share and var are
  ledir="$MY_PWD/sbin"			# Where mysqld is
# Since we didn't find anything, used the compiled-in defaults
else
  MY_BASEDIR_VERSION='@prefix@'
  ledir='@libexecdir@'
fi

helper=`find_in_bin mariadbd-safe-helper`

print_defaults=`find_in_bin my_print_defaults`
# Check if helper exists
command -v $helper --help >/dev/null 2>&1
helper_exist=$?
#
# Second, try to find the data directory
#

# Try where the binary installs put it
if test -d $MY_BASEDIR_VERSION/data/mysql
then
  DATADIR=$MY_BASEDIR_VERSION/data
# Next try where the source installs put it
elif test -d $MY_BASEDIR_VERSION/var/mysql
then
  DATADIR=$MY_BASEDIR_VERSION/var
# Or just give up and use our compiled-in default
else
  DATADIR=@localstatedir@
fi

if test -z "$MYSQL_HOME"
then
  if test -r "$DATADIR/my.cnf"
  then
    log_error "WARNING: Found $DATADIR/my.cnf
The data directory is not a valid location for my.cnf, please move it to
$MY_BASEDIR_VERSION/my.cnf"
  fi
  MYSQL_HOME=$MY_BASEDIR_VERSION
fi
export MYSQL_HOME

append_arg_to_args () {
  args="$args "`shell_quote_string "$1"`
}

args=

# Get first arguments from the my.cnf file, groups [mysqld] and [server]
# (and related) and then merge with the command line arguments

SET_USER=2
parse_arguments `$print_defaults $defaults --loose-verbose --mysqld`
if test $SET_USER -eq 2
then
  SET_USER=0
fi

# If arguments come from [mysqld_safe] section of my.cnf
# we complain about unrecognized options
unrecognized_handling=complain
parse_arguments `$print_defaults $defaults --loose-verbose mysqld_safe safe_mysqld mariadb_safe mariadbd-safe`

# We only need to pass arguments through to the server if we don't
# handle them here.  So, we collect unrecognized options (passed on
# the command line) into the args variable.
unrecognized_handling=collect
parse_arguments "$@"


#
# Try to find the plugin directory
#

# Use user-supplied argument
if [ -n "${PLUGIN_DIR}" ]; then
  plugin_dir="${PLUGIN_DIR}"
else
  # Try to find plugin dir relative to basedir
  for dir in lib64/mysql/plugin lib64/plugin lib/mysql/plugin lib/plugin
  do
    if [ -d "${MY_BASEDIR_VERSION}/${dir}" ]; then
      plugin_dir="${MY_BASEDIR_VERSION}/${dir}"
      break
    fi
  done
  # Give up and use compiled-in default
  if [ -z "${plugin_dir}" ]; then
    plugin_dir='@pkgplugindir@'
  fi
fi
plugin_dir="${plugin_dir}${PLUGIN_VARIANT}"

# Determine what logging facility to use

# Ensure that 'logger' exists, if it's requested
if [ $want_syslog -eq 1 ]
then
  if ! command -v logger > /dev/null
  then
    log_error "--syslog requested, but no 'logger' program found.  Please ensure that 'logger' is in your PATH, or do not specify the --syslog option to mysqld_safe."
    exit 1
  fi
fi

if [ $skip_err_log -eq 1 ]
then
  append_arg_to_args "--skip-log-error"
fi

if [ -n "$err_log" -o $want_syslog -eq 0 ]
then
  if [ -n "$err_log" ]
  then
    # mysqld adds ".err" if there is no extension on the --log-error
    # argument; must match that here, or mysqld_safe will write to a
    # different log file than mysqld

    # mysqld does not add ".err" to "--log-error=foo."; it considers a
    # trailing "." as an extension

    if expr "$err_log" : '.*\.[^/]*$' > /dev/null
    then
        :
    else
      err_log="$err_log".err
    fi

    case "$err_log" in
      /* ) ;;
      * ) err_log="$DATADIR/$err_log" ;;
    esac
  else
    if [ -n "$err_log_base" ]
    then
      err_log=$err_log_base.err
      case "$err_log" in
        /* ) ;;
        * ) err_log="$DATADIR/$err_log" ;;
      esac
    else
      err_log=$DATADIR/`@HOSTNAME@`.err
    fi
  fi

  append_arg_to_args "--log-error=$err_log"

  if [ $want_syslog -eq 1 ]
  then
    # User explicitly asked for syslog, so warn that it isn't used
    log_error "Can't log to error log and syslog at the same time.  Remove all --log-error configuration options for --syslog to take effect."
    want_syslog=0
  fi

  # Log to err_log file
  log_notice "Logging to '$err_log'."
  logging=file

else
  if [ -n "$syslog_tag" ]
  then
    # Sanitize the syslog tag
    syslog_tag=`echo "$syslog_tag" | sed -e 's/[^a-zA-Z0-9_-]/_/g'`
    syslog_tag_mysqld_safe="${syslog_tag_mysqld_safe}-$syslog_tag"
    syslog_tag_mysqld="${syslog_tag_mysqld}-$syslog_tag"
  fi
  log_notice "Logging to syslog."
  logging=syslog
fi

USER_OPTION=""
if test -w / -o "$USER" = "root"
then
  if test "$user" != "root" -o $SET_USER = 1
  then
    USER_OPTION="--user=$user"
    # To be used if/when we enable --system-group as an option to mysqld
    GROUP_OPTION="--group=$group"
  fi
  if test -n "$open_files"
  then
    ulimit -n $open_files
  fi
fi

if test -n "$open_files"
then
  append_arg_to_args "--open-files-limit=$open_files"
fi

safe_mysql_unix_port=${mysql_unix_port:-${MYSQL_UNIX_PORT:-@MYSQL_UNIX_ADDR@}}
# Make sure that directory for $safe_mysql_unix_port exists
mysql_unix_port_dir=`dirname $safe_mysql_unix_port`
if [ ! -d $mysql_unix_port_dir -a $dry_run -eq 0 ]
then
  if ! mkdir -p $mysql_unix_port_dir
  then
    log_error "Fatal error Can't create database directory '$mysql_unix_port'"
    exit 1
  fi
  if [ "$user" -a "$group" ]; then
    chown $user:$group $mysql_unix_port_dir
  else
    [ "$user" ] && chown $user $mysql_unix_port_dir
    [ "$group" ] && chgrp $group $mysql_unix_port_dir
  fi
  chmod 755 $mysql_unix_port_dir
fi

# If the user doesn't specify a binary, we assume name "mariadbd"
if test -z "$MYSQLD"
then
  MYSQLD=mariadbd
fi

if test ! -x "$ledir/$MYSQLD"
then
  log_error "The file $ledir/$MYSQLD
does not exist or is not executable. Please cd to the mysql installation
directory and restart this script from there as follows:
./bin/mysqld_safe&
See https://mariadb.com/kb/en/mysqld_safe for more information"
  exit 1
fi

if test -z "$pid_file"
then
  pid_file="`@HOSTNAME@`.pid"
fi
# MariaDB wants pid file without datadir
append_arg_to_args "--pid-file=$pid_file"
case "$pid_file" in
  /* ) ;;
  * )  pid_file="$DATADIR/$pid_file" ;;
esac

if test -n "$mysql_unix_port"
then
  append_arg_to_args "--socket=$mysql_unix_port"
fi
if test -n "$mysql_tcp_port"
then
  append_arg_to_args "--port=$mysql_tcp_port"
fi

if test $niceness -eq 0
then
  NOHUP_NICENESS="nohup"
else
  NOHUP_NICENESS="nohup nice -$niceness"
fi

# Using nice with no args to get the niceness level is GNU-specific.
# This check could be extended for other operating systems (e.g.,
# BSD could use "nohup sh -c 'ps -o nice -p $$' | tail -1").
# But, it also seems that GNU nohup is the only one which messes
# with the priority, so this is okay.
if nohup nice > /dev/null 2>&1
then
    normal_niceness=`nice`
    nohup_niceness=`nohup nice 2>/dev/null`

    numeric_nice_values=1
    for val in $normal_niceness $nohup_niceness
    do
        case "$val" in
            -[0-9] | -[0-9][0-9] | -[0-9][0-9][0-9] | \
             [0-9] |  [0-9][0-9] |  [0-9][0-9][0-9] )
                ;;
            * )
                numeric_nice_values=0 ;;
        esac
    done

    if test $numeric_nice_values -eq 1
    then
        nice_value_diff=`expr $nohup_niceness - $normal_niceness`
        if test $? -eq 0 && test $nice_value_diff -gt 0 && \
            nice --$nice_value_diff echo testing > /dev/null 2>&1
        then
            # nohup increases the priority (bad), and we are permitted
            # to lower the priority with respect to the value the user
            # might have been given
            niceness=`expr $niceness - $nice_value_diff`
            NOHUP_NICENESS="nice -$niceness nohup"
        fi
    fi
else
    if nohup echo testing > /dev/null 2>&1
    then
        :
    else
        # nohup doesn't work on this system
        NOHUP_NICENESS=""
    fi
fi

# Try to set the core file size (even if we aren't root) because many systems
# don't specify a hard limit on core file size.
if test -n "$core_file_size"
then
  ulimit -c $core_file_size
fi

#
# If there exists an old pid file, check if the daemon is already running
# Note: The switches to 'ps' may depend on your operating system
if test -f "$pid_file" && [ $dry_run -eq 0 ]
then
  PID=`cat "$pid_file"`
  if @CHECK_PID@
  then
    if @FIND_PROC@
    then    # The pid contains a mysqld process
      log_error "A mysqld process already exists"
      exit 1
    fi
  fi
  rm -f "$pid_file"
  if test -f "$pid_file"
  then
    log_error "Fatal error: Can't remove the pid file:
$pid_file
Please remove it manually and start $0 again;
mysqld daemon not started"
    exit 1
  fi
fi

#
# Flush and purge buffers/caches.
#

if @TARGET_LINUX@ && test $flush_caches -eq 1
then
  # Locate sync, ensure it exists.
  if ! command -v sync > /dev/null
  then
    log_error "sync command not found, required for --flush-caches"
    exit 1
  # Flush file system buffers.
  elif ! sync
  then
    # Huh, the sync() function is always successful...
    log_error "sync failed, check if sync is properly installed"
  fi

  # Locate sysctl, ensure it exists.
  if ! command -v sysctl > /dev/null
  then
    log_error "sysctl command not found, required for --flush-caches"
    exit 1
  # Purge page cache, dentries and inodes.
  elif ! sysctl -q -w vm.drop_caches=3
  then
    log_error "sysctl failed, check the error message for details"
    exit 1
  fi
elif test $flush_caches -eq 1
then
  log_error "--flush-caches is not supported on this platform"
  exit 1
fi

#
# Uncomment the following lines if you want all tables to be automatically
# checked and repaired during startup. You should add sensible key_buffer
# and sort_buffer values to my.cnf to improve check performance or require
# less disk space.
# Alternatively, you can start mysqld with the "myisam-recover" option. See
# the manual for details.
#
# echo "Checking tables in $DATADIR"
# $MY_BASEDIR_VERSION/bin/myisamchk --silent --force --fast --medium-check $DATADIR/*/*.MYI
# $MY_BASEDIR_VERSION/bin/isamchk --silent --force $DATADIR/*/*.ISM

# Does this work on all systems?
#if type ulimit | grep "shell builtin" > /dev/null
#then
#  ulimit -n 256 > /dev/null 2>&1		# Fix for BSD and FreeBSD systems
#fi

cmd="`mysqld_ld_preload_text`$NOHUP_NICENESS"
[ $dry_run -eq 1 ] && cmd=''

#
# Set mysqld's memory interleave policy.
#

if @TARGET_LINUX@ && test $numa_interleave -eq 1
then
  # Locate numactl, ensure it exists.
  if ! command -v numactl > /dev/null
  then
    log_error "numactl command not found, required for --numa-interleave"
    exit 1
  # Attempt to run a command, ensure it works.
  elif ! numactl --interleave=all true
  then
    log_error "numactl failed, check if numactl is properly installed"
  fi

  # Launch mysqld with numactl.
  [ $dry_run -eq 0 ] && cmd="$cmd numactl --interleave=all"
elif test $numa_interleave -eq 1
then
  log_error "--numa-interleave is not supported on this platform"
  exit 1
fi

for i in  "$ledir/$MYSQLD" "$defaults_group_suffix" "$defaults" "--basedir=$MY_BASEDIR_VERSION" \
  "--datadir=$DATADIR" "--plugin-dir=$plugin_dir" "$USER_OPTION"
do
  cmd="$cmd "`shell_quote_string "$i"`
done
cmd="$cmd $args"

if [ $dry_run -eq 1 ]
then
  # RETURN or EXIT depending if the script is being sourced or not.
  (return 2> /dev/null) && return || exit
fi


# Avoid 'nohup: ignoring input' warning
test -n "$NOHUP_NICENESS" && cmd="$cmd < /dev/null"
log_notice "Starting $MYSQLD daemon with databases from $DATADIR"

# variable to track the current number of "fast" (a.k.a. subsecond) restarts
fast_restart=0
# maximum number of restarts before trottling kicks in
max_fast_restarts=5
# flag whether a usable sleep command exists
have_sleep=1

# close stdout and stderr, everything goes to $logging now
if expr "${-}" : '.*x' > /dev/null
then
  :
else
  exec 1>/dev/null
  exec 2>/dev/null
fi

# maximum number of wsrep restarts
max_wsrep_restarts=0

while true
do
  rm -f "$pid_file"	# Some extra safety

  start_time=`date +%M%S`

  # Perform wsrep position recovery if wsrep_on=1, skip otherwise.
  if test $wsrep_on -eq 1
  then
    # this sets wsrep_start_position_opt
    wsrep_recover_position "$cmd"

    [ $? -ne 0 ] && exit 1 #

    [ -n "$wsrep_urls" ] && url=`wsrep_pick_url $wsrep_urls` # check connect address

    if [ -z "$url" ]
    then
      eval_log_error "$cmd $wsrep_start_position_opt"
    else
      eval_log_error "$cmd $wsrep_start_position_opt --wsrep_cluster_address=$url"
    fi
  else
    eval_log_error "$cmd"
  fi
  end_time=`date +%M%S`

  if test ! -f "$pid_file"		# This is removed if normal shutdown
  then
    break
  fi


  # sanity check if time reading is sane and there's sleep
  if test $end_time -gt 0 -a $have_sleep -gt 0
  then
    # throttle down the fast restarts
    if test $end_time -eq $start_time
    then
      fast_restart=`expr $fast_restart + 1`
      if test $fast_restart -ge $max_fast_restarts
      then
        log_notice "The server is respawning too fast. Sleeping for 1 second."
        sleep 1
        sleep_state=$?
        if test $sleep_state -gt 0
        then
          log_notice "The server is respawning too fast and no working sleep command. Turning off trottling."
          have_sleep=0
        fi

        fast_restart=0
      fi
    else
      fast_restart=0
    fi
  fi

  if @TARGET_LINUX@ && test $KILL_MYSQLD -eq 1
  then
    # Test if one process was hanging.
    # This is only a fix for Linux (running as base 3 mysqld processes)
    # but should work for the rest of the servers.
    # The only thing is ps x => redhat 5 gives warnings when using ps -x.
    # kill -9 is used or the process won't react on the kill.
    numofproces=`ps xaww | grep -v "grep" | grep "$ledir/$MYSQLD\>" | grep -c "pid-file=$pid_file"`

    log_notice "Number of processes running now: $numofproces"
    I=1
    while test "$I" -le "$numofproces"
    do
      PROC=`ps xaww | grep "$ledir/$MYSQLD\>" | grep -v "grep" | grep "pid-file=$pid_file" | sed -n '$p'`

      for T in $PROC
      do
        break
      done
      #    echo "TEST $I - $T **"
      if kill -9 $T
      then
        log_error "$MYSQLD process hanging, pid $T - killed"
      else
        break
      fi
      I=`expr $I + 1`
    done
  fi

  if [ -n "$wsrep_restart" ]
  then
    if [ $wsrep_restart -le $max_wsrep_restarts ]
    then
      wsrep_restart=`expr $wsrep_restart + 1`
      log_notice "WSREP: sleeping 15 seconds before restart"
      sleep 15
    else
      log_notice "WSREP: not restarting wsrep node automatically"
      break
    fi
  fi

  log_notice "mysqld restarted"
  if test -n "$crash_script"
  then
    crash_script_output=`$crash_script 2>&1`
    log_error "$crash_script_output"
  fi
done

log_notice "mysqld from pid file $pid_file ended"
