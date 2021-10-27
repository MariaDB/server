#!/usr/bin/env perl
# -*- cperl -*-

# Copyright (c) 2008, 2013, Oracle and/or its affiliates. All rights reserved.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA

##############################################################################
#
#  mysql-test-run.pl
#
#  Tool used for executing a suite of .test file
#
#  See the "MySQL Test framework manual" for more information
#  https://mariadb.com/kb/en/library/mysqltest/
#
#  Please keep the test framework tools identical in all versions!
#
##############################################################################
#
# Coding style directions for this perl script
#
#   - To make this Perl script easy to alter even for those that not
#     code Perl that often, keeep the coding style as close as possible to
#     the C/C++ MySQL coding standard.
#
#   - All lists of arguments to send to commands are Perl lists/arrays,
#     not strings we append args to. Within reason, most string
#     concatenation for arguments should be avoided.
#
#   - Functions defined in the main program are not to be prefixed,
#     functions in "library files" are to be prefixed with "mtr_" (for
#     Mysql-Test-Run). There are some exceptions, code that fits best in
#     the main program, but are put into separate files to avoid
#     clutter, may be without prefix.
#
#   - All stat/opendir/-f/ is to be kept in collect_test_cases(). It
#     will create a struct that the rest of the program can use to get
#     the information. This separates the "find information" from the
#     "do the work" and makes the program more easy to maintain.
#
#   - The rule when it comes to the logic of this program is
#
#       command_line_setup() - is to handle the logic between flags
#       collect_test_cases() - is to do its best to select what tests
#                              to run, dig out options, if needs restart etc.
#       run_testcase()       - is to run a single testcase, and follow the
#                              logic set in both above. No, or rare file
#                              system operations. If a test seems complex,
#                              it should probably not be here.
#
# A nice way to trace the execution of this script while debugging
# is to use the Devel::Trace package found at
# "http://www.plover.com/~mjd/perl/Trace/" and run this script like
# "perl -d:Trace mysql-test-run.pl"
#


use lib "lib/v1/";

$Devel::Trace::TRACE= 0;       # Don't trace boring init stuff

#require 5.6.1;
use File::Path;
use File::Basename;
use File::Copy;
use File::Temp qw /tempdir/;
use File::Spec::Functions qw /splitdir/;
use Cwd;
use Getopt::Long;
use IO::Socket;
use IO::Socket::INET;
use strict;
use warnings;

select(STDOUT);
$| = 1; # Automatically flush STDOUT

our $glob_win32_perl=  ($^O eq "MSWin32"); # ActiveState Win32 Perl
our $glob_cygwin_perl= ($^O eq "cygwin");  # Cygwin Perl
our $glob_win32=       ($glob_win32_perl or $glob_cygwin_perl);

require "lib/v1/mtr_cases.pl";
require "lib/v1/mtr_process.pl";
require "lib/v1/mtr_timer.pl";
require "lib/v1/mtr_io.pl";
require "lib/v1/mtr_gcov.pl";
require "lib/v1/mtr_gprof.pl";
require "lib/v1/mtr_report.pl";
require "lib/v1/mtr_match.pl";
require "lib/v1/mtr_misc.pl";
require "lib/v1/mtr_stress.pl";
require "lib/v1/mtr_unique.pl";

$Devel::Trace::TRACE= 1;

##############################################################################
#
#  Default settings
#
##############################################################################

# Misc global variables
our $mysql_version_id;
our $glob_mysql_test_dir=         undef;
our $glob_mysql_bench_dir=        undef;
our $glob_scriptname=             undef;
our $glob_timers=                 undef;
our $glob_use_embedded_server=    0;
our @glob_test_mode;

our $glob_basedir;
our $glob_bindir;

our $path_charsetsdir;
our $path_client_bindir;
our $path_client_libdir;
our $path_share;
our $path_language;
our $path_timefile;
our $path_snapshot;
our $path_mysqltest_log;
our $path_current_test_log;

our $opt_vardir;                 # A path but set directly on cmd line
our $path_vardir_trace;          # unix formatted opt_vardir for trace files
our $opt_tmpdir;                 # A path but set directly on cmd line

# Visual Studio produces executables in different sub-directories based on the
# configuration used to build them.  To make life easier, an environment
# variable or command-line option may be specified to control which set of
# executables will be used by the test suite.
our $multiconfig = $ENV{'MTR_VS_CONFIG'};

our $default_vardir;

our $opt_usage;
our $opt_list_options;
our $opt_suites;
our $opt_suites_default= "main,binlog,rpl,maria"; # Default suites to run
our $opt_script_debug= 0;  # Script debugging, enable with --script-debug
our $opt_verbose= 0;  # Verbose output, enable with --verbose

our $exe_master_mysqld;
our $exe_mysql;
our $exe_mysqladmin;
our $exe_mysql_upgrade;
our $exe_mysqlbinlog;
our $exe_mysql_client_test;
our $exe_bug25714;
our $exe_mysqld;
our $exe_mysqlcheck;
our $exe_mysqldump;
our $exe_mysqlslap;
our $exe_mysqlimport;
our $exe_mysqlshow;
our $file_mysql_fix_privilege_tables;
our $exe_mysqltest;
our $exe_slave_mysqld;
our $exe_my_print_defaults;
our $exe_perror;
our $lib_udf_example;
our $lib_example_plugin;
our $exe_libtool;

our $opt_bench= 0;
our $opt_small_bench= 0;
our $opt_big_test= 0;

our @opt_combinations;
our $opt_skip_combination;

our @opt_extra_mysqld_opt;
our @opt_extra_mysqltest_opt;

our $opt_compress;
our $opt_ssl;
our $opt_skip_ssl;
our $opt_ssl_supported;
our $opt_ps_protocol;
our $opt_sp_protocol;
our $opt_cursor_protocol;
our $opt_view_protocol;

our $opt_debug;
our $opt_do_test;
our @opt_cases;                  # The test cases names in argv
our $opt_embedded_server;

our $opt_extern= 0;
our $opt_socket;

our $opt_fast;
our $opt_force;
our $opt_reorder= 0;
our $opt_enable_disabled;
our $opt_mem= $ENV{'MTR_MEM'};

our $opt_gcov;
our $opt_gcov_err;
our $opt_gcov_msg;

our $glob_debugger= 0;
our $opt_gdb;
our $opt_client_gdb;
our $opt_ddd;
our $opt_client_ddd;
our $opt_manual_gdb;
our $opt_manual_ddd;
our $opt_manual_debug;
our $opt_mtr_build_thread=0;
our $opt_debugger;
our $opt_client_debugger;

our $opt_gprof;
our $opt_gprof_dir;
our $opt_gprof_master;
our $opt_gprof_slave;

our $master;
our $slave;

our $opt_master_myport;
our $opt_slave_myport;

our $opt_record;
my $opt_report_features;
our $opt_check_testcases;
our $opt_mark_progress;

our $opt_skip_rpl;
our $max_slave_num= 0;
our $max_master_num= 1;
our $use_innodb;
our $opt_skip_test;

our $opt_sleep;

our $opt_testcase_timeout;
our $opt_suite_timeout;
my  $default_testcase_timeout=     15; # 15 min max
my  $default_suite_timeout=       300; # 5 hours max

our $opt_start_and_exit;
our $opt_start_dirty;
our $opt_start_from;

our $opt_strace_client;

our $opt_timer= 1;

our $opt_user;

my $opt_valgrind= 0;
my $opt_valgrind_mysqld= 0;
my $opt_valgrind_mysqltest= 0;
my @default_valgrind_args= ("--show-reachable=yes");
my @valgrind_args;
my $opt_valgrind_path;
my $opt_callgrind;

our $opt_stress=               "";
our $opt_stress_suite=     "main";
our $opt_stress_mode=    "random";
our $opt_stress_threads=        5;
our $opt_stress_test_count=     0;
our $opt_stress_loop_count=     0;
our $opt_stress_test_duration=  0;
our $opt_stress_init_file=     "";
our $opt_stress_test_file=     "";

our $opt_warnings;

our $opt_skip_master_binlog= 0;
our $opt_skip_slave_binlog= 0;

our $path_sql_dir;

our @data_dir_lst;

our $used_binlog_format;
our $used_default_engine;
our $debug_compiled_binaries;

our %mysqld_variables;

my $source_dist= 0;

our $opt_max_save_core= 5;
my $num_saved_cores= 0;  # Number of core files saved in vardir/log/ so far.

######################################################################
#
#  Function declarations
#
######################################################################

sub main ();
sub initial_setup ();
sub command_line_setup ();
sub set_mtr_build_thread_ports($);
sub datadir_list_setup ();
sub executable_setup ();
sub environment_setup ();
sub kill_running_servers ();
sub remove_stale_vardir ();
sub setup_vardir ();
sub check_ssl_support ($);
sub check_running_as_root();
sub mysqld_wait_started($);
sub run_benchmarks ($);
sub initialize_servers ();
sub mysql_install_db ();
sub install_db ($$);
sub copy_install_db ($$);
sub run_testcase ($);
sub run_testcase_stop_servers ($$$);
sub run_testcase_start_servers ($);
sub run_testcase_check_skip_test($);
sub report_failure_and_restart ($);
sub do_before_start_master ($);
sub do_before_start_slave ($);
sub mysqld_start ($$$);
sub mysqld_arguments ($$$$);
sub stop_all_servers ();
sub run_mysqltest ($);
sub usage ($);


######################################################################
#
#  Main program
#
######################################################################

main();

sub main () {

  command_line_setup();

  check_ssl_support(\%mysqld_variables);
  check_debug_support(\%mysqld_variables);

  executable_setup();

  environment_setup();
  signal_setup();

  if ( $opt_gcov )
  {
    gcov_prepare();
  }

  if ( $opt_gprof )
  {
    gprof_prepare();
  }

  if ( $opt_bench )
  {
    initialize_servers();
    run_benchmarks(shift);      # Shift what? Extra arguments?!
  }
  elsif ( $opt_stress )
  {
    initialize_servers();
    run_stress_test()
  }
  else
  {
    # Figure out which tests we are going to run
    if (!$opt_suites)
    {
      $opt_suites= $opt_suites_default;
    }

    my $tests= collect_test_cases($opt_suites);

    my ($need_debug);
    foreach my $test (@$tests)
    {
      next if $test->{skip};

      if (!$opt_extern)
      {
        $need_debug||=$test->{need_debug};

	# Count max number of slaves used by a test case
	if ( $test->{slave_num} > $max_slave_num) {
	  $max_slave_num= $test->{slave_num};
	  mtr_error("Too many slaves") if $max_slave_num > 3;
	}

	# Count max number of masters used by a test case
	if ( $test->{master_num} > $max_master_num) {
	  $max_master_num= $test->{master_num};
	  mtr_error("Too many masters") if $max_master_num > 2;
	  mtr_error("Too few masters") if $max_master_num < 1;
	}
      }
      $use_innodb||= $test->{'innodb_test'};
    }

    if ( !$need_debug && !$opt_debug)
    {
      $opt_debug=0;
    }

    initialize_servers();

    if ( $opt_report_features ) {
      run_report_features();
    }

    run_tests($tests);
  }

  mtr_exit(0);
}

##############################################################################
#
#  Default settings
#
##############################################################################

#
# When an option is no longer used by this program, it must be explicitly
# ignored or else it will be passed through to mysqld.  GetOptions will call
# this subroutine once for each such option on the command line.  See
# Getopt::Long documentation.
#

sub warn_about_removed_option {
  my ($option, $value, $hash_value) = @_;

  warn "WARNING: This option is no longer used, and is ignored: --$option\n";
}

sub command_line_setup () {

  # These are defaults for things that are set on the command line

  my $opt_comment;

  # Magic number -69.4 results in traditional test ports starting from 9306.
  set_mtr_build_thread_ports(-69.4);

  # If so requested, we try to avail ourselves of a unique build thread number.
  if ( $ENV{'MTR_BUILD_THREAD'} ) {
    if ( lc($ENV{'MTR_BUILD_THREAD'}) eq 'auto' ) {
      print "Requesting build thread... ";
      $ENV{'MTR_BUILD_THREAD'} = mtr_require_unique_id_and_wait("/tmp/mysql-test-ports", 200, 299);
      print "got ".$ENV{'MTR_BUILD_THREAD'}."\n";
    }
  }

  if ( $ENV{'MTR_BUILD_THREAD'} )
  {
    set_mtr_build_thread_ports($ENV{'MTR_BUILD_THREAD'});
  }

  # This is needed for test log evaluation in "gen-build-status-page"
  # in all cases where the calling tool does not log the commands
  # directly before it executes them, like "make test-force-pl" in RPM builds.
  print "Logging: $0 ", join(" ", @ARGV), "\n";

  # Read the command line
  # Note: Keep list, and the order, in sync with usage at end of this file

  # Options that are no longer used must still be processed, because all
  # unprocessed options are passed directly to mysqld.  The user will be
  # warned that the option is being ignored.
  #
  # Put the complete option string here.  For example, to remove the --suite
  # option, remove it from GetOptions() below and put 'suite|suites=s' here.
  my @removed_options = (
  );

  Getopt::Long::Configure("pass_through");
  my %options=(
             # Control what engine/variation to run
             'embedded-server'          => \$opt_embedded_server,
             'ps-protocol'              => \$opt_ps_protocol,
             'sp-protocol'              => \$opt_sp_protocol,
             'view-protocol'            => \$opt_view_protocol,
             'cursor-protocol'          => \$opt_cursor_protocol,
             'ssl|with-openssl'         => \$opt_ssl,
             'skip-ssl'                 => \$opt_skip_ssl,
             'compress'                 => \$opt_compress,
             'bench'                    => \$opt_bench,
             'small-bench'              => \$opt_small_bench,
             'vs-config'            => \$multiconfig,

             # Control what test suites or cases to run
             'force'                    => \$opt_force,
             'skip-master-binlog'       => \$opt_skip_master_binlog,
             'skip-slave-binlog'        => \$opt_skip_slave_binlog,
             'do-test=s'                => \$opt_do_test,
             'start-from=s'             => \$opt_start_from,
             'suite|suites=s'           => \$opt_suites,
             'skip-rpl'                 => \$opt_skip_rpl,
             'skip-test=s'              => \$opt_skip_test,
             'big-test'                 => \$opt_big_test,
             'combination=s'            => \@opt_combinations,
             'skip-combination'         => \$opt_skip_combination,

             # Specify ports
             'master_port=i'            => \$opt_master_myport,
             'slave_port=i'             => \$opt_slave_myport,
	     'mtr-build-thread=i'       => \$opt_mtr_build_thread,

             # Test case authoring
             'record'                   => \$opt_record,
             'check-testcases'          => \$opt_check_testcases,
             'mark-progress'            => \$opt_mark_progress,

             # Extra options used when starting mysqld
             'mysqld=s'                 => \@opt_extra_mysqld_opt,

             # Extra options used when starting mysqld
             'mysqltest=s'                 => \@opt_extra_mysqltest_opt,

             # Run test on running server
             'extern'                   => \$opt_extern,

             # Debugging
             'gdb'                      => \$opt_gdb,
             'client-gdb'               => \$opt_client_gdb,
             'manual-gdb'               => \$opt_manual_gdb,
             'manual-debug'             => \$opt_manual_debug,
             'ddd'                      => \$opt_ddd,
             'client-ddd'               => \$opt_client_ddd,
             'manual-ddd'               => \$opt_manual_ddd,
	     'debugger=s'               => \$opt_debugger,
	     'client-debugger=s'        => \$opt_client_debugger,
             'strace-client'            => \$opt_strace_client,
             'master-binary=s'          => \$exe_master_mysqld,
             'slave-binary=s'           => \$exe_slave_mysqld,
             'max-save-core=i'          => \$opt_max_save_core,

             # Coverage, profiling etc
             'gcov'                     => \$opt_gcov,
             'gprof'                    => \$opt_gprof,
             'valgrind|valgrind-all'    => \$opt_valgrind,
             'valgrind-mysqltest'       => \$opt_valgrind_mysqltest,
             'valgrind-mysqld'          => \$opt_valgrind_mysqld,
             'valgrind-options=s'       => sub {
	       my ($opt, $value)= @_;
	       # Deprecated option unless it's what we know pushbuild uses
	       if ($value eq "--gen-suppressions=all --show-reachable=yes") {
		 push(@valgrind_args, $_) for (split(' ', $value));
		 return;
	       }
	       die("--valgrind-options=s is deprecated. Use ",
		   "--valgrind-option=s, to be specified several",
		   " times if necessary");
	     },
             'valgrind-option=s'        => \@valgrind_args,
             'valgrind-path=s'          => \$opt_valgrind_path,
	     'callgrind'                => \$opt_callgrind,

             # Stress testing
             'stress'                   => \$opt_stress,
             'stress-suite=s'           => \$opt_stress_suite,
             'stress-threads=i'         => \$opt_stress_threads,
             'stress-test-file=s'       => \$opt_stress_test_file,
             'stress-init-file=s'       => \$opt_stress_init_file,
             'stress-mode=s'            => \$opt_stress_mode,
             'stress-loop-count=i'      => \$opt_stress_loop_count,
             'stress-test-count=i'      => \$opt_stress_test_count,
             'stress-test-duration=i'   => \$opt_stress_test_duration,

	     # Directories
             'tmpdir=s'                 => \$opt_tmpdir,
             'vardir=s'                 => \$opt_vardir,
             'benchdir=s'               => \$glob_mysql_bench_dir,
             'mem'                      => \$opt_mem,
             'client-bindir=s'          => \$path_client_bindir,
             'client-libdir=s'          => \$path_client_libdir,

             # Misc
             'report-features'          => \$opt_report_features,
             'comment=s'                => \$opt_comment,
             'debug'                    => \$opt_debug,
             'fast'                     => \$opt_fast,
             'reorder'                  => \$opt_reorder,
             'enable-disabled'          => \$opt_enable_disabled,
             'script-debug'             => \$opt_script_debug,
             'verbose'                  => \$opt_verbose,
             'sleep=i'                  => \$opt_sleep,
             'socket=s'                 => \$opt_socket,
             'start-dirty'              => \$opt_start_dirty,
             'start-and-exit'           => \$opt_start_and_exit,
             'timer!'                   => \$opt_timer,
             'user=s'                   => \$opt_user,
             'testcase-timeout=i'       => \$opt_testcase_timeout,
             'suite-timeout=i'          => \$opt_suite_timeout,
             'warnings|log-warnings'    => \$opt_warnings,

             # Options which are no longer used
             (map { $_ => \&warn_about_removed_option } @removed_options),

             'help|h'                   => \$opt_usage,
             'list-options'             => \$opt_list_options,
            );

  GetOptions(%options) or usage("Can't read options");

  usage("") if $opt_usage;
  list_options(\%options) if $opt_list_options;

  $glob_scriptname=  basename($0);

  if ($opt_mtr_build_thread != 0)
  {
    set_mtr_build_thread_ports($opt_mtr_build_thread)
  }
  elsif ($ENV{'MTR_BUILD_THREAD'})
  {
    $opt_mtr_build_thread= $ENV{'MTR_BUILD_THREAD'};
  }

  # We require that we are in the "mysql-test" directory
  # to run mysql-test-run
  if (! -f $glob_scriptname)
  {
    mtr_error("Can't find the location for the mysql-test-run script\n" .
              "Go to the mysql-test directory and execute the script " .
              "as follows:\n./$glob_scriptname");
  }

  if ( -d "../sql" )
  {
    $source_dist=  1;
  }

  # Find the absolute path to the test directory
  $glob_mysql_test_dir=  cwd();
  if ( $glob_cygwin_perl )
  {
    # Windows programs like 'mysqld' needs Windows paths
    $glob_mysql_test_dir= `cygpath -m "$glob_mysql_test_dir"`;
    chomp($glob_mysql_test_dir);
  }
  if (defined $ENV{MTR_BINDIR})
  {
    $default_vardir= "$ENV{MTR_BINDIR}/mysql-test/var";
  }
  else
  {
    $default_vardir= "$glob_mysql_test_dir/var";
  }

  # In most cases, the base directory we find everything relative to,
  # is the parent directory of the "mysql-test" directory. For source
  # distributions, TAR binary distributions and some other packages.
  $glob_basedir= dirname($glob_mysql_test_dir);

  $glob_bindir= $ENV{'MTR_BINDIR'} || $glob_basedir;
  # In the RPM case, binaries and libraries are installed in the
  # default system locations, instead of having our own private base
  # directory. And we install "/usr/share/mysql-test". Moving up one
  # more directory relative to "mysql-test" gives us a usable base
  # directory for RPM installs.
  if ( ! $source_dist and ! -d "$glob_basedir/bin" )
  {
    $glob_basedir= dirname($glob_basedir);
  }

  # Expect mysql-bench to be located adjacent to the source tree, by default
  $glob_mysql_bench_dir= "$glob_basedir/../mysql-bench"
    unless defined $glob_mysql_bench_dir;
  $glob_mysql_bench_dir= undef
    unless -d $glob_mysql_bench_dir;


  $glob_timers= mtr_init_timers();

  # --------------------------------------------------------------------------
  # Embedded server flag
  # --------------------------------------------------------------------------
  if ( $opt_embedded_server )
  {
    $glob_use_embedded_server= 1;
    # Add the location for libmysqld.dll to the path.
    if ( $glob_win32 )
    {
      my $lib_mysqld=
        mtr_path_exists(vs_config_dirs('libmysqld',''));
	  $lib_mysqld= $glob_cygwin_perl ? ":".`cygpath "$lib_mysqld"`
                                     : ";".$lib_mysqld;
      chomp($lib_mysqld);
      $ENV{'PATH'}="$ENV{'PATH'}".$lib_mysqld;
    }

    push(@glob_test_mode, "embedded");
    $opt_skip_rpl= 1;              # We never run replication with embedded
    $opt_skip_ssl= 1;              # Turn off use of SSL

    # Turn off use of bin log
    push(@opt_extra_mysqld_opt, "--skip-log-bin");

    if ( $opt_extern )
    {
      mtr_error("Can't use --extern with --embedded-server");
    }
  }

  #
  # Find the mysqld executable to be able to find the mysqld version
  # number as early as possible
  #

  # Look for the client binaries directory
  if ($path_client_bindir)
  {
    # --client-bindir=path set on command line, check that the path exists
    $path_client_bindir= mtr_path_exists($path_client_bindir);
  }
  else
  {
    $path_client_bindir= mtr_path_exists("$glob_bindir/client_release",
					 "$glob_bindir/client_debug",
					 vs_config_dirs('client', ''),
					 "$glob_bindir/client",
					 "$glob_bindir/bin");
  }

  # Look for language files and charsetsdir, use same share
  $path_share=      mtr_path_exists("$glob_bindir/share/mysql",
                                    "$glob_bindir/sql/share",
                                    "$glob_bindir/share");

  $path_language=      mtr_path_exists("$path_share");
  $path_charsetsdir =   mtr_path_exists("$glob_basedir/share/mysql/charsets",
                                    "$glob_basedir/sql/share/charsets",
                                    "$glob_basedir/share/charsets");

  if (!$opt_extern)
  {
    $exe_mysqld=       mtr_exe_exists (vs_config_dirs('sql', 'mysqld'),
                                       vs_config_dirs('sql', 'mysqld-debug'),
				       "$glob_bindir/sql/mysqld",
				       "$path_client_bindir/mysqld-max-nt",
				       "$path_client_bindir/mysqld-max",
				       "$path_client_bindir/mysqld-nt",
				       "$path_client_bindir/mysqld",
				       "$path_client_bindir/mysqld-debug",
				       "$path_client_bindir/mysqld-max",
				       "$glob_bindir/libexec/mysqld",
				       "$glob_bindir/bin/mariadbd",
				       "$glob_bindir/sbin/mariadbd");

    # Use the mysqld found above to find out what features are available
    collect_mysqld_features();
  }
  else
  {
    $mysqld_variables{'port'}= 3306;
    $mysqld_variables{'master-port'}= 3306;
  }

  if ( $opt_comment )
  {
    print "\n";
    print '#' x 78, "\n";
    print "# $opt_comment\n";
    print '#' x 78, "\n\n";
  }

  foreach my $arg ( @ARGV )
  {
    if ( $arg =~ /^--skip-/ )
    {
      push(@opt_extra_mysqld_opt, $arg);
    }
    elsif ( $arg =~ /^--$/ )
    {
      # It is an effect of setting 'pass_through' in option processing
      # that the lone '--' separating options from arguments survives,
      # simply ignore it.
    }
    elsif ( $arg =~ /^-/ )
    {
      usage("Invalid option \"$arg\"");
    }
    else
    {
      push(@opt_cases, $arg);
    }
  }

  # --------------------------------------------------------------------------
  # Find out type of logging that are being used
  # --------------------------------------------------------------------------
  if (!$opt_extern && $mysql_version_id >= 50100 )
  {
    foreach my $arg ( @opt_extra_mysqld_opt )
    {
      if ( $arg =~ /binlog[-_]format=(\S+)/ )
      {
      	$used_binlog_format= $1;
      }
    }
    if (defined $used_binlog_format)
    {
      mtr_report("Using binlog format '$used_binlog_format'");
    }
    else
    {
      mtr_report("Using dynamic switching of binlog format");
    }
  }


  # --------------------------------------------------------------------------
  # Find out default storage engine being used(if any)
  # --------------------------------------------------------------------------
  foreach my $arg ( @opt_extra_mysqld_opt )
  {
    if ( $arg =~ /default-storage-engine=(\S+)/ )
    {
      $used_default_engine= $1;
    }
  }
  mtr_report("Using default engine '$used_default_engine'")
    if defined $used_default_engine;

  if ($glob_win32 and defined $opt_mem) {
    mtr_report("--mem not supported on Windows, ignored");
    $opt_mem= undef;
  }

  # --------------------------------------------------------------------------
  # Check if we should speed up tests by trying to run on tmpfs
  # --------------------------------------------------------------------------
  if ( defined $opt_mem )
  {
    mtr_error("Can't use --mem and --vardir at the same time ")
      if $opt_vardir;
    mtr_error("Can't use --mem and --tmpdir at the same time ")
      if $opt_tmpdir;

    # Search through list of locations that are known
    # to be "fast disks" to list to find a suitable location
    # Use --mem=<dir> as first location to look.
    my @tmpfs_locations= ($opt_mem, "/dev/shm", "/tmp");

    foreach my $fs (@tmpfs_locations)
    {
      if ( -d $fs )
      {
	mtr_report("Using tmpfs in $fs");
	$opt_mem= "$fs/var";
	$opt_mem .= $opt_mtr_build_thread if $opt_mtr_build_thread;
	last;
      }
    }
  }

  # --------------------------------------------------------------------------
  # Set the "var/" directory, as it is the base for everything else
  # --------------------------------------------------------------------------
  if ( ! $opt_vardir )
  {
    $opt_vardir= $default_vardir;
  }
  elsif ( $mysql_version_id < 50000 and
	  $opt_vardir ne $default_vardir)
  {
    # Version 4.1 and --vardir was specified
    # Only supported as a symlink from var/
    # by setting up $opt_mem that symlink will be created
    if ( ! $glob_win32 )
    {
      # Only platforms that have native symlinks can use the vardir trick
      $opt_mem= $opt_vardir;
      mtr_report("Using 4.1 vardir trick");
    }

    $opt_vardir= $default_vardir;
  }

  $path_vardir_trace= $opt_vardir;
  # Chop off any "c:", DBUG likes a unix path ex: c:/src/... => /src/...
  $path_vardir_trace=~ s/^\w://;

  # We make the path absolute, as the server will do a chdir() before usage
  unless ( $opt_vardir =~ m,^/, or
           ($glob_win32 and $opt_vardir =~ m,^[a-z]:/,i) )
  {
    # Make absolute path, relative test dir
    $opt_vardir= "$glob_mysql_test_dir/$opt_vardir";
  }

  # --------------------------------------------------------------------------
  # Set tmpdir
  # --------------------------------------------------------------------------
  $opt_tmpdir=       "$opt_vardir/tmp" unless $opt_tmpdir;
  $opt_tmpdir =~ s,/+$,,;       # Remove ending slash if any

  # --------------------------------------------------------------------------
  # Record flag
  # --------------------------------------------------------------------------
  if ( $opt_record and ! @opt_cases )
  {
    mtr_error("Will not run in record mode without a specific test case");
  }

  if ( $opt_record )
  {
    $opt_skip_combination = 1;
  }

  # --------------------------------------------------------------------------
  # ps protcol flag
  # --------------------------------------------------------------------------
  if ( $opt_ps_protocol )
  {
    push(@glob_test_mode, "ps-protocol");
  }

  # --------------------------------------------------------------------------
  # Bench flags
  # --------------------------------------------------------------------------
  if ( $opt_small_bench )
  {
    $opt_bench=  1;
  }

  # --------------------------------------------------------------------------
  # Big test flags
  # --------------------------------------------------------------------------
   if ( $opt_big_test )
   {
     $ENV{'BIG_TEST'}= 1;
   }

  # --------------------------------------------------------------------------
  # Gcov flag
  # --------------------------------------------------------------------------
  if ( $opt_gcov and ! $source_dist )
  {
    mtr_error("Coverage test needs the source - please use source dist");
  }

  # --------------------------------------------------------------------------
  # Check debug related options
  # --------------------------------------------------------------------------
  if ( $opt_gdb || $opt_client_gdb || $opt_ddd || $opt_client_ddd ||
       $opt_manual_gdb || $opt_manual_ddd || $opt_manual_debug ||
       $opt_debugger || $opt_client_debugger )
  {
    # Indicate that we are using debugger
    $glob_debugger= 1;
    if ( $opt_extern )
    {
      mtr_error("Can't use --extern when using debugger");
    }
  }

  # --------------------------------------------------------------------------
  # Check if special exe was selected for master or slave
  # --------------------------------------------------------------------------
  $exe_master_mysqld= $exe_master_mysqld || $exe_mysqld;
  $exe_slave_mysqld=  $exe_slave_mysqld  || $exe_mysqld;

  # --------------------------------------------------------------------------
  # Check valgrind arguments
  # --------------------------------------------------------------------------
  if ( $opt_valgrind or $opt_valgrind_path or @valgrind_args)
  {
    mtr_report("Turning on valgrind for all executables");
    $opt_valgrind= 1;
    $opt_valgrind_mysqld= 1;
    $opt_valgrind_mysqltest= 1;
  }
  elsif ( $opt_valgrind_mysqld )
  {
    mtr_report("Turning on valgrind for mysqld(s) only");
    $opt_valgrind= 1;
  }
  elsif ( $opt_valgrind_mysqltest )
  {
    mtr_report("Turning on valgrind for mysqltest and mysql_client_test only");
    $opt_valgrind= 1;
  }

  if ( $opt_callgrind )
  {
    mtr_report("Turning on valgrind with callgrind for mysqld(s)");
    $opt_valgrind= 1;
    $opt_valgrind_mysqld= 1;

    # Set special valgrind options unless options passed on command line
    push(@valgrind_args, "--trace-children=yes")
      unless @valgrind_args;
  }

  if ( $opt_valgrind )
  {
    # Set valgrind_options to default unless already defined
    push(@valgrind_args, @default_valgrind_args)
      unless @valgrind_args;

    mtr_report("Running valgrind with options \"",
	       join(" ", @valgrind_args), "\"");
  }

  if ( ! $opt_testcase_timeout )
  {
    $opt_testcase_timeout=
      $ENV{MTR_TESTCASE_TIMEOUT} || $default_testcase_timeout;
    $opt_testcase_timeout*= 10 if $opt_valgrind;
    $opt_testcase_timeout*= 10 if ($opt_debug and $glob_win32);
  }

  if ( ! $opt_suite_timeout )
  {
    $opt_suite_timeout=
      $ENV{MTR_SUITE_TIMEOUT} || $default_suite_timeout;
    $opt_suite_timeout*= 6 if $opt_valgrind;
    $opt_suite_timeout*= 6 if ($opt_debug and $glob_win32);
  }

  if ( ! $opt_user )
  {
    if ( $opt_extern )
    {
      $opt_user= "test";
    }
    else
    {
      $opt_user= "root"; # We want to do FLUSH xxx commands
    }
  }

  # On QNX, /tmp/dir/master.sock and /tmp/dir//master.sock seem to be
  # considered different, so avoid the extra slash (/) in the socket
  # paths.
  my $sockdir = $opt_tmpdir;
  $sockdir =~ s|/+$||;

  # On some operating systems, there is a limit to the length of a
  # UNIX domain socket's path far below PATH_MAX, so try to avoid long
  # socket path names.
  $sockdir = tempdir(CLEANUP => 0) if ( length($sockdir) >= 70 );

  $master->[0]=
  {
   pid           => 0,
   type          => "master",
   idx           => 0,
   path_myddir   => "$opt_vardir/master-data",
   path_myerr    => "$opt_vardir/log/master.err",
   path_pid    => "$opt_vardir/run/master.pid",
   path_sock   => "$sockdir/master.sock",
   port   =>  $opt_master_myport,
   start_timeout =>  400, # enough time create innodb tables
   start_opts    => [],
  };

  $master->[1]=
  {
   pid           => 0,
   type          => "master",
   idx           => 1,
   path_myddir   => "$opt_vardir/master1-data",
   path_myerr    => "$opt_vardir/log/master1.err",
   path_pid    => "$opt_vardir/run/master1.pid",
   path_sock   => "$sockdir/master1.sock",
   port   => $opt_master_myport + 1,
   start_timeout => 400, # enough time create innodb tables
   start_opts    => [],
  };

  $slave->[0]=
  {
   pid           => 0,
   type          => "slave",
   idx           => 0,
   path_myddir   => "$opt_vardir/slave-data",
   path_myerr    => "$opt_vardir/log/slave.err",
   path_pid    => "$opt_vardir/run/slave.pid",
   path_sock   => "$sockdir/slave.sock",
   port   => $opt_slave_myport,
   start_timeout => 400,

   start_opts    => [],
  };

  $slave->[1]=
  {
   pid           => 0,
   type          => "slave",
   idx           => 1,
   path_myddir   => "$opt_vardir/slave1-data",
   path_myerr    => "$opt_vardir/log/slave1.err",
   path_pid    => "$opt_vardir/run/slave1.pid",
   path_sock   => "$sockdir/slave1.sock",
   port   => $opt_slave_myport + 1,
   start_timeout => 300,
   start_opts    => [],
  };

  $slave->[2]=
  {
   pid           => 0,
   type          => "slave",
   idx           => 2,
   path_myddir   => "$opt_vardir/slave2-data",
   path_myerr    => "$opt_vardir/log/slave2.err",
   path_pid    => "$opt_vardir/run/slave2.pid",
   path_sock   => "$sockdir/slave2.sock",
   port   => $opt_slave_myport + 2,
   start_timeout => 300,
   start_opts    => [],
  };

  # --------------------------------------------------------------------------
  # extern
  # --------------------------------------------------------------------------
  if ( $opt_extern )
  {
    # Turn off features not supported when running with extern server
    $opt_skip_rpl= 1;

    # Setup master->[0] with the settings for the extern server
    $master->[0]->{'path_sock'}=  $opt_socket ? $opt_socket : "/tmp/mysql.sock";
    mtr_report("Using extern server at '$master->[0]->{path_sock}'");
  }
  else
  {
    mtr_error("--socket can only be used in combination with --extern")
      if $opt_socket;
  }



  $path_timefile=  "$opt_vardir/log/mysqltest-time";
  $path_mysqltest_log=  "$opt_vardir/log/mysqltest.log";
  $path_current_test_log= "$opt_vardir/log/current_test";

  $path_snapshot= "$opt_tmpdir/snapshot_$opt_master_myport/";
}

#
# To make it easier for different devs to work on the same host,
# an environment variable can be used to control all ports. A small
# number is to be used, 0 - 16 or similar.
#
# Note the MASTER_MYPORT has to be set the same in all 4.x and 5.x
# versions of this script, else a 4.0 test run might conflict with a
# 5.1 test run, even if different MTR_BUILD_THREAD is used. This means
# all port numbers might not be used in this version of the script.
#
# Also note the limitation of ports we are allowed to hand out. This
# differs between operating systems and configuration, see
# http://www.ncftp.com/ncftpd/doc/misc/ephemeral_ports.html
# But a fairly safe range seems to be 5001 - 32767
#

sub set_mtr_build_thread_ports($) {
  my $mtr_build_thread= shift;

  if ( lc($mtr_build_thread) eq 'auto' ) {
    print "Requesting build thread... ";
    $ENV{'MTR_BUILD_THREAD'} = $mtr_build_thread = mtr_require_unique_id_and_wait("/tmp/mysql-test-ports", 200, 299);
    print "got ".$mtr_build_thread."\n";
  }

  # Up to two masters, up to three slaves
  # A magic value in command_line_setup depends on these equations.
  $opt_master_myport=         $mtr_build_thread * 10 + 10000; # and 1
  $opt_slave_myport=          $opt_master_myport + 2;  # and 3 4

  if ( $opt_master_myport < 5001 or $opt_master_myport + 10 >= 32767 )
  {
    mtr_error("MTR_BUILD_THREAD number results in a port",
              "outside 5001 - 32767",
              "($opt_master_myport - $opt_master_myport + 10)");
  }
}


sub datadir_list_setup () {

  # Make a list of all data_dirs
  for (my $idx= 0; $idx < $max_master_num; $idx++)
  {
    push(@data_dir_lst, $master->[$idx]->{'path_myddir'});
  }

  for (my $idx= 0; $idx < $max_slave_num; $idx++)
  {
    push(@data_dir_lst, $slave->[$idx]->{'path_myddir'});
  }
}


##############################################################################
#
#  Set paths to various executable programs
#
##############################################################################


sub collect_mysqld_features () {
  my $found_variable_list_start= 0;
  my $tmpdir;
  if ( $opt_tmpdir ) {
    # Use the requested tmpdir
    mkpath($opt_tmpdir) if (! -d $opt_tmpdir);
    $tmpdir= $opt_tmpdir;
  }
  else {
    $tmpdir= tempdir(CLEANUP => 0); # Directory removed by this function
  }

  #
  # Execute "mysqld --help --verbose" to get a list
  # list of all features and settings
  #
  # --no-defaults and --skip-grant-tables are to avoid loading
  # system-wide configs and plugins
  #
  # --datadir must exist, mysqld will chdir into it
  #
  my $list= `$exe_mysqld --no-defaults --datadir=$tmpdir --lc-messages-dir=$path_language --skip-grant-tables --verbose --help`;

  foreach my $line (split('\n', $list))
  {
    # First look for version
    if ( !$mysql_version_id )
    {
      # Look for version
      my $exe_name= basename($exe_mysqld);
      mtr_verbose("exe_name: $exe_name");
      if ( $line =~ /^\S*$exe_name\s\sVer\s([0-9]*)\.([0-9]*)\.([0-9]*)/ )
      {
	#print "Major: $1 Minor: $2 Build: $3\n";
	$mysql_version_id= $1*10000 + $2*100 + $3;
	#print "mysql_version_id: $mysql_version_id\n";
	mtr_report("MySQL Version $1.$2.$3");
      }
    }
    else
    {
      if (!$found_variable_list_start)
      {
	# Look for start of variables list
	if ( $line =~ /[\-]+\s[\-]+/ )
	{
	  $found_variable_list_start= 1;
	}
      }
      else
      {
	# Put variables into hash
	if ( $line =~ /^([\S]+)[ \t]+(.*?)\r?$/ )
	{
	  # print "$1=\"$2\"\n";
	  $mysqld_variables{$1}= $2;
	}
	else
	{
	  # The variable list is ended with a blank line
	  if ( $line =~ /^[\s]*$/ )
	  {
	    last;
	  }
	  else
	  {
	    # Send out a warning, we should fix the variables that has no
	    # space between variable name and it's value
	    # or should it be fixed width column parsing? It does not
	    # look like that in function my_print_variables in my_getopt.c
	    mtr_warning("Could not parse variable list line : $line");
	  }
	}
      }
    }
  }
  rmtree($tmpdir) if (!$opt_tmpdir);
  mtr_error("Could not find version of MySQL") unless $mysql_version_id;
  mtr_error("Could not find variabes list") unless $found_variable_list_start;

}


sub run_query($$) {
  my ($mysqld, $query)= @_;

  my $args;
  mtr_init_args(\$args);

  mtr_add_arg($args, "--no-defaults");
  mtr_add_arg($args, "--user=%s", $opt_user);
  mtr_add_arg($args, "--port=%d", $mysqld->{'port'});
  mtr_add_arg($args, "--socket=%s", $mysqld->{'path_sock'});
  mtr_add_arg($args, "--silent"); # Tab separated output
  mtr_add_arg($args, "-e '%s'", $query);

  my $cmd= "$exe_mysql " . join(' ', @$args);
  mtr_verbose("cmd: $cmd");
  return `$cmd`;
}


sub collect_mysqld_features_from_running_server ()
{
  my $list= run_query($master->[0], "use mysql; SHOW VARIABLES");

  foreach my $line (split('\n', $list))
  {
    # Put variables into hash
    if ( $line =~ /^([\S]+)[ \t]+(.*?)\r?$/ )
    {
      print "$1=\"$2\"\n";
      $mysqld_variables{$1}= $2;
    }
  }
}

sub executable_setup () {

  #
  # Check if libtool is available in this distribution/clone
  # we need it when valgrinding or debugging non installed binary
  # Otherwise valgrind will valgrind the libtool wrapper or bash
  # and gdb will not find the real executable to debug
  #
  if ( -x "../libtool")
  {
    $exe_libtool= "../libtool";
    if ($opt_valgrind or $glob_debugger)
    {
      mtr_report("Using \"$exe_libtool\" when running valgrind or debugger");
    }
  }

  # Look for my_print_defaults
  $exe_my_print_defaults=
    mtr_exe_exists(vs_config_dirs('extra', 'my_print_defaults'),
		           "$path_client_bindir/my_print_defaults",
		           "$glob_bindir/extra/my_print_defaults");

  # Look for perror
  $exe_perror= mtr_exe_exists(vs_config_dirs('extra', 'perror'),
			                  "$glob_bindir/extra/perror",
			                  "$path_client_bindir/perror");

  # Look for the client binaries
  $exe_mysqlcheck=     mtr_exe_exists("$path_client_bindir/mysqlcheck");
  $exe_mysqldump=      mtr_exe_exists("$path_client_bindir/mysqldump");
  $exe_mysqlimport=    mtr_exe_exists("$path_client_bindir/mysqlimport");
  $exe_mysqlshow=      mtr_exe_exists("$path_client_bindir/mysqlshow");
  $exe_mysqlbinlog=    mtr_exe_exists("$path_client_bindir/mysqlbinlog");
  $exe_mysqladmin=     mtr_exe_exists("$path_client_bindir/mysqladmin");
  $exe_mysql=          mtr_exe_exists("$path_client_bindir/mysql");

  if (!$opt_extern)
  {
    # Look for SQL scripts directory
    if ( mtr_file_exists("$path_share/mysql_system_tables.sql") ne "")
    {
      # The SQL scripts are in path_share
      $path_sql_dir= $path_share;
    }
    else
    {
      $path_sql_dir= mtr_path_exists("$glob_basedir/share",
				     "$glob_basedir/scripts");
    }

    if ( $mysql_version_id >= 50100 )
    {
      $exe_mysqlslap=    mtr_exe_exists("$path_client_bindir/mysqlslap");
    }
    if ( $mysql_version_id >= 50000 and !$glob_use_embedded_server )
    {
      $exe_mysql_upgrade= mtr_exe_exists("$path_client_bindir/mysql_upgrade")
    }
    else
    {
      $exe_mysql_upgrade= "";
    }

    # Look for mysql_fix_privilege_tables.sql script
    $file_mysql_fix_privilege_tables=
      mtr_file_exists("$glob_basedir/scripts/mysql_fix_privilege_tables.sql",
  		    "$glob_basedir/share/mysql_fix_privilege_tables.sql",
  		    "$glob_basedir/share/mysql/mysql_fix_privilege_tables.sql");

    # Look for the udf_example library
    $lib_udf_example=
      mtr_file_exists(vs_config_dirs('sql', 'udf_example.dll'),
                      "$glob_bindir/sql/.libs/udf_example.so",);

    # Look for the ha_example library
    $lib_example_plugin=
      mtr_file_exists(vs_config_dirs('storage/example', 'ha_example.dll'),
                      "$glob_bindir/storage/example/.libs/ha_example.so",);

  }

  # Look for mysqltest executable
  if ( $glob_use_embedded_server )
  {
    $exe_mysqltest=
      mtr_exe_exists(vs_config_dirs('libmysqld/examples','mysqltest_embedded'),
                     "$glob_bindir/libmysqld/examples/mysqltest_embedded",
                     "$path_client_bindir/mysqltest_embedded");
  }
  else
  {
    $exe_mysqltest= mtr_exe_exists("$path_client_bindir/mysqltest");
  }

  # Look for mysql_client_test executable which may _not_ exist in
  # some versions, test using it should be skipped
  if ( $glob_use_embedded_server )
  {
    $exe_mysql_client_test=
      mtr_exe_maybe_exists(
        vs_config_dirs('libmysqld/examples', 'mysql_client_test_embedded'),
        "$glob_bindir/libmysqld/examples/mysql_client_test_embedded");
  }
  else
  {
    $exe_mysql_client_test=
      mtr_exe_maybe_exists(vs_config_dirs('tests', 'mysql_client_test'),
                           "$glob_bindir/tests/mysql_client_test",
                           "$glob_bindir/bin/mysql_client_test");
  }

  # Look for bug25714 executable which may _not_ exist in
  # some versions, test using it should be skipped
  $exe_bug25714=
      mtr_exe_maybe_exists(vs_config_dirs('tests', 'bug25714'),
                           "$glob_bindir/tests/bug25714");
}


sub generate_cmdline_mysqldump ($) {
  my($mysqld) = @_;
  return
    mtr_native_path($exe_mysqldump) .
      " --no-defaults -uroot --debug-check " .
      "--port=$mysqld->{'port'} " .
      "--socket=$mysqld->{'path_sock'} --password=";
}


##############################################################################
#
#  Set environment to be used by childs of this process for
#  things that are constant duting the whole lifetime of mysql-test-run.pl
#
##############################################################################

sub mysql_client_test_arguments()
{
  my $exe= $exe_mysql_client_test;

  my $args;
  mtr_init_args(\$args);
  if ( $opt_valgrind_mysqltest )
  {
    valgrind_arguments($args, \$exe);
  }

  mtr_add_arg($args, "--no-defaults");
  mtr_add_arg($args, "--testcase");
  mtr_add_arg($args, "--user=root");
  mtr_add_arg($args, "--port=$master->[0]->{'port'}");
  mtr_add_arg($args, "--socket=$master->[0]->{'path_sock'}");

  if ( $opt_extern || $mysql_version_id >= 50000 )
  {
    mtr_add_arg($args, "--vardir=$opt_vardir")
  }

  if ( $opt_debug )
  {
    mtr_add_arg($args,
      "--debug=d:t:A,$path_vardir_trace/log/mysql_client_test.trace");
  }

  if ( $glob_use_embedded_server )
  {
    mtr_add_arg($args,
      " -A --lc-messages-dir=$path_language");
    mtr_add_arg($args,
      " -A --datadir=$slave->[0]->{'path_myddir'}");
    mtr_add_arg($args,
      " -A --character-sets-dir=$path_charsetsdir");
  }

  return join(" ", $exe, @$args);
}

sub mysql_upgrade_arguments()
{
  my $exe= $exe_mysql_upgrade;

  my $args;
  mtr_init_args(\$args);
#  if ( $opt_valgrind_mysql_ugrade )
#  {
#    valgrind_arguments($args, \$exe);
#  }

  mtr_add_arg($args, "--no-defaults");
  mtr_add_arg($args, "--user=root");
  mtr_add_arg($args, "--port=$master->[0]->{'port'}");
  mtr_add_arg($args, "--socket=$master->[0]->{'path_sock'}");
  mtr_add_arg($args, "--datadir=$master->[0]->{'path_myddir'}");
  mtr_add_arg($args, "--basedir=$glob_basedir");
  mtr_add_arg($args, "--tmpdir=$opt_tmpdir");

  if ( $opt_debug )
  {
    mtr_add_arg($args,
      "--debug=d:t:A,$path_vardir_trace/log/mysql_upgrade.trace");
  }

  return join(" ", $exe, @$args);
}

# Note that some env is setup in spawn/run, in "mtr_process.pl"

sub environment_setup () {

  umask(022);

  my @ld_library_paths;

  if ($path_client_libdir)
  {
    # Use the --client-libdir passed on commandline
    push(@ld_library_paths, "$path_client_libdir");
  }
  else
  {
    # Setup LD_LIBRARY_PATH so the libraries from this distro/clone
    # are used in favor of the system installed ones
    if ( $source_dist )
    {
      push(@ld_library_paths, "$glob_bindir/libmysql/.libs/",
	   "$glob_bindir/libmysql_r/.libs/",
	   "$glob_bindir/zlib.libs/");
    }
    else
    {
      push(@ld_library_paths, "$glob_bindir/lib");
    }
  }

  # --------------------------------------------------------------------------
  # Valgrind need to be run with debug libraries otherwise it's almost
  # impossible to add correct supressions, that means if "/usr/lib/debug"
  # is available, it should be added to
  # LD_LIBRARY_PATH
  #
  # But pthread is broken in libc6-dbg on Debian <= 3.1 (see Debian
  # bug 399035, http://bugs.debian.org/cgi-bin/bugreport.cgi?bug=399035),
  # so don't change LD_LIBRARY_PATH on that platform.
  # --------------------------------------------------------------------------
  my $debug_libraries_path= "/usr/lib/debug";
  my $deb_version;
  if (  $opt_valgrind and -d $debug_libraries_path and
        (! -e '/etc/debian_version' or
	 ($deb_version= mtr_grab_file('/etc/debian_version')) !~ /^[0-9]+\.[0-9]$/ or
         $deb_version > 3.1 ) )
  {
    push(@ld_library_paths, $debug_libraries_path);
  }

  $ENV{'LD_LIBRARY_PATH'}= join(":", @ld_library_paths,
				$ENV{'LD_LIBRARY_PATH'} ?
				split(':', $ENV{'LD_LIBRARY_PATH'}) : ());
  mtr_debug("LD_LIBRARY_PATH: $ENV{'LD_LIBRARY_PATH'}");

  $ENV{'DYLD_LIBRARY_PATH'}= join(":", @ld_library_paths,
				  $ENV{'DYLD_LIBRARY_PATH'} ?
				  split(':', $ENV{'DYLD_LIBRARY_PATH'}) : ());
  mtr_debug("DYLD_LIBRARY_PATH: $ENV{'DYLD_LIBRARY_PATH'}");

  # The environment variable used for shared libs on AIX
  $ENV{'SHLIB_PATH'}= join(":", @ld_library_paths,
                           $ENV{'SHLIB_PATH'} ?
                           split(':', $ENV{'SHLIB_PATH'}) : ());
  mtr_debug("SHLIB_PATH: $ENV{'SHLIB_PATH'}");

  # The environment variable used for shared libs on hp-ux
  $ENV{'LIBPATH'}= join(":", @ld_library_paths,
                        $ENV{'LIBPATH'} ?
                        split(':', $ENV{'LIBPATH'}) : ());
  mtr_debug("LIBPATH: $ENV{'LIBPATH'}");

  # --------------------------------------------------------------------------
  # Also command lines in .opt files may contain env vars
  # --------------------------------------------------------------------------

  $ENV{'CHARSETSDIR'}=              $path_charsetsdir;
  $ENV{'UMASK'}=              "0660"; # The octal *string*
  $ENV{'UMASK_DIR'}=          "0770"; # The octal *string*

  #
  # MySQL tests can produce output in various character sets
  # (especially, ctype_xxx.test). To avoid confusing Perl
  # with output which is incompatible with the current locale
  # settings, we reset the current values of LC_ALL and LC_CTYPE to "C".
  # For details, please see
  # Bug#27636 tests fails if LC_* variables set to *_*.UTF-8
  #
  $ENV{'LC_ALL'}=             "C";
  $ENV{'LC_CTYPE'}=           "C";

  $ENV{'LC_COLLATE'}=         "C";
  $ENV{'USE_RUNNING_SERVER'}= $opt_extern;
  $ENV{'MYSQL_TEST_DIR'}=     $glob_mysql_test_dir;
  $ENV{'MYSQLTEST_VARDIR'}=   $opt_vardir;
  $ENV{'MYSQL_TMP_DIR'}=      $opt_tmpdir;
  $ENV{'MASTER_MYSOCK'}=      $master->[0]->{'path_sock'};
  $ENV{'MASTER_MYSOCK1'}=     $master->[1]->{'path_sock'};
  $ENV{'MASTER_MYPORT'}=      $master->[0]->{'port'};
  $ENV{'MASTER_MYPORT1'}=     $master->[1]->{'port'};
  $ENV{'SLAVE_MYSOCK'}=       $slave->[0]->{'path_sock'};
  $ENV{'SLAVE_MYPORT'}=       $slave->[0]->{'port'};
  $ENV{'SLAVE_MYPORT1'}=      $slave->[1]->{'port'};
  $ENV{'SLAVE_MYPORT2'}=      $slave->[2]->{'port'};
  $ENV{'MYSQL_TCP_PORT'}=     $mysqld_variables{'port'};
  $ENV{'DEFAULT_MASTER_PORT'}= $mysqld_variables{'master-port'};

  $ENV{MTR_BUILD_THREAD}=      $opt_mtr_build_thread;

  $ENV{'EXE_MYSQL'}=          $exe_mysql;

  # ----------------------------------------------------
  # Setup env so childs can execute mysqlcheck
  # ----------------------------------------------------
  my $cmdline_mysqlcheck=
    mtr_native_path($exe_mysqlcheck) .
    " --no-defaults --debug-check -uroot " .
    "--port=$master->[0]->{'port'} " .
    "--socket=$master->[0]->{'path_sock'} --password=";

  if ( $opt_debug )
  {
    $cmdline_mysqlcheck .=
      " --debug=d:t:A,$path_vardir_trace/log/mysqlcheck.trace";
  }
  $ENV{'MYSQL_CHECK'}=              $cmdline_mysqlcheck;

  # ----------------------------------------------------
  # Setup env to childs can execute myqldump
  # ----------------------------------------------------
  my $cmdline_mysqldump= generate_cmdline_mysqldump($master->[0]);
  my $cmdline_mysqldumpslave= generate_cmdline_mysqldump($slave->[0]);

  if ( $opt_debug )
  {
    $cmdline_mysqldump .=
      " --debug=d:t:A,$path_vardir_trace/log/mysqldump-master.trace";
    $cmdline_mysqldumpslave .=
      " --debug=d:t:A,$path_vardir_trace/log/mysqldump-slave.trace";
  }
  $ENV{'MYSQL_DUMP'}= $cmdline_mysqldump;
  $ENV{'MYSQL_DUMP_SLAVE'}= $cmdline_mysqldumpslave;


  # ----------------------------------------------------
  # Setup env so childs can execute mysqlslap
  # ----------------------------------------------------
  if ( $exe_mysqlslap )
  {
    my $cmdline_mysqlslap=
      mtr_native_path($exe_mysqlslap) .
      " -uroot " .
      "--port=$master->[0]->{'port'} " .
      "--socket=$master->[0]->{'path_sock'} --password= ";

    if ( $opt_debug )
   {
      $cmdline_mysqlslap .=
	" --debug=d:t:A,$path_vardir_trace/log/mysqlslap.trace";
    }
    $ENV{'MYSQL_SLAP'}= $cmdline_mysqlslap;
  }

  # ----------------------------------------------------
  # Setup env so childs can execute mysqlimport
  # ----------------------------------------------------
  my $cmdline_mysqlimport=
    mtr_native_path($exe_mysqlimport) .
    " -uroot --debug-check " .
    "--port=$master->[0]->{'port'} " .
    "--socket=$master->[0]->{'path_sock'} --password=";

  if ( $opt_debug )
  {
    $cmdline_mysqlimport .=
      " --debug=d:t:A,$path_vardir_trace/log/mysqlimport.trace";
  }
  $ENV{'MYSQL_IMPORT'}= $cmdline_mysqlimport;


  # ----------------------------------------------------
  # Setup env so childs can execute mysqlshow
  # ----------------------------------------------------
  my $cmdline_mysqlshow=
    mtr_native_path($exe_mysqlshow) .
    " -uroot --debug-check " .
    "--port=$master->[0]->{'port'} " .
    "--socket=$master->[0]->{'path_sock'} --password=";

  if ( $opt_debug )
  {
    $cmdline_mysqlshow .=
      " --debug=d:t:A,$path_vardir_trace/log/mysqlshow.trace";
  }
  $ENV{'MYSQL_SHOW'}= $cmdline_mysqlshow;

  # ----------------------------------------------------
  # Setup env so childs can execute mysqlbinlog
  # ----------------------------------------------------
  my $cmdline_mysqlbinlog=
    mtr_native_path($exe_mysqlbinlog) .
      " --no-defaults --disable-force-if-open --debug-check";
  if ( !$opt_extern && $mysql_version_id >= 50000 )
  {
    $cmdline_mysqlbinlog .=" --character-sets-dir=$path_charsetsdir";
  }
  # Always use the given tmpdir for the LOAD files created
  # by mysqlbinlog
  $cmdline_mysqlbinlog .=" --local-load=$opt_tmpdir";

  if ( $opt_debug )
  {
    $cmdline_mysqlbinlog .=
      " --debug=d:t:A,$path_vardir_trace/log/mysqlbinlog.trace";
  }
  $ENV{'MYSQL_BINLOG'}= $cmdline_mysqlbinlog;

  # ----------------------------------------------------
  # Setup env so childs can execute mysql
  # ----------------------------------------------------
  my $cmdline_mysql=
    mtr_native_path($exe_mysql) .
    " --no-defaults --debug-check --host=localhost  --user=root --password= " .
    "--port=$master->[0]->{'port'} " .
    "--socket=$master->[0]->{'path_sock'} ".
    "--character-sets-dir=$path_charsetsdir";

  $ENV{'MYSQL'}= $cmdline_mysql;

  # ----------------------------------------------------
  # Setup env so childs can execute bug25714
  # ----------------------------------------------------
  $ENV{'MYSQL_BUG25714'}=  $exe_bug25714;

  # ----------------------------------------------------
  # Setup env so childs can execute mysql_client_test
  # ----------------------------------------------------
  $ENV{'MYSQL_CLIENT_TEST'}=  mysql_client_test_arguments();

  # ----------------------------------------------------
  # Setup env so childs can execute mysql_upgrade
  # ----------------------------------------------------
  if ( !$opt_extern && $mysql_version_id >= 50000 )
  {
    $ENV{'MYSQL_UPGRADE'}= mysql_upgrade_arguments();
  }

  if ( !$opt_extern )
  {
    $ENV{'MYSQL_FIX_PRIVILEGE_TABLES'}=  $file_mysql_fix_privilege_tables;
  }

  # ----------------------------------------------------
  # Setup env so childs can execute my_print_defaults
  # ----------------------------------------------------
  $ENV{'MYSQL_MY_PRINT_DEFAULTS'}= mtr_native_path($exe_my_print_defaults);

  # ----------------------------------------------------
  # Setup env so childs can execute mysqladmin
  # ----------------------------------------------------
  $ENV{'MYSQLADMIN'}= mtr_native_path($exe_mysqladmin);

  # ----------------------------------------------------
  # Setup env so childs can execute perror
  # ----------------------------------------------------
  $ENV{'MY_PERROR'}= mtr_native_path($exe_perror);

  # ----------------------------------------------------
  # Add the path where mysqld will find udf_example.so
  # ----------------------------------------------------
  $ENV{'UDF_EXAMPLE_LIB'}=
    ($lib_udf_example ? basename($lib_udf_example) : "");
  $ENV{'UDF_EXAMPLE_LIB_OPT'}=
    ($lib_udf_example ? "--plugin_dir=" . dirname($lib_udf_example) : "");

  # ----------------------------------------------------
  # Add the path where mysqld will find ha_example.so
  # ----------------------------------------------------
  $ENV{'EXAMPLE_PLUGIN'}=
    ($lib_example_plugin ? basename($lib_example_plugin) : "");
  $ENV{'EXAMPLE_PLUGIN_OPT'}=
    ($lib_example_plugin ? "--plugin_dir=" . dirname($lib_example_plugin) : "");

  # ----------------------------------------------------
  # Setup env so childs can execute myisampack and myisamchk
  # ----------------------------------------------------
  $ENV{'MYISAMCHK'}= mtr_native_path(mtr_exe_exists(
                       vs_config_dirs('storage/myisam', 'myisamchk'),
                       vs_config_dirs('myisam', 'myisamchk'),
                       "$path_client_bindir/myisamchk",
                       "$glob_bindir/storage/myisam/myisamchk",
                       "$glob_bindir/myisam/myisamchk"));
  $ENV{'MYISAMPACK'}= mtr_native_path(mtr_exe_exists(
                        vs_config_dirs('storage/myisam', 'myisampack'),
                        vs_config_dirs('myisam', 'myisampack'),
                        "$path_client_bindir/myisampack",
                        "$glob_bindir/storage/myisam/myisampack",
                        "$glob_bindir/myisam/myisampack"));

  # ----------------------------------------------------
  # Setup env so childs can execute aria_pack and aria_chk
  # ----------------------------------------------------
  $ENV{'ARIA_CHK'}= mtr_native_path(mtr_exe_maybe_exists(
                       vs_config_dirs('storage/maria', 'aria_chk'),
                       vs_config_dirs('maria', 'aria_chk'),
                       "$path_client_bindir/aria_chk",
                       "$glob_basedir/storage/maria/aria_chk",
                       "$glob_basedir/maria/aria_chk"));
  $ENV{'ARIA_PACK'}= mtr_native_path(mtr_exe_maybe_exists(
                        vs_config_dirs('storage/maria', 'aria_pack'),
                        vs_config_dirs('maria', 'aria_pack'),
                        "$path_client_bindir/aria_pack",
                        "$glob_basedir/storage/maria/aria_pack",
                        "$glob_basedir/maria/aria_pack"));

  # ----------------------------------------------------
  # We are nice and report a bit about our settings
  # ----------------------------------------------------
  if (!$opt_extern)
  {
    print "Using MTR_BUILD_THREAD      = $ENV{MTR_BUILD_THREAD}\n";
    print "Using MASTER_MYPORT         = $ENV{MASTER_MYPORT}\n";
    print "Using MASTER_MYPORT1        = $ENV{MASTER_MYPORT1}\n";
    print "Using SLAVE_MYPORT          = $ENV{SLAVE_MYPORT}\n";
    print "Using SLAVE_MYPORT1         = $ENV{SLAVE_MYPORT1}\n";
    print "Using SLAVE_MYPORT2         = $ENV{SLAVE_MYPORT2}\n";
  }

  # Create an environment variable to make it possible
  # to detect that valgrind is being used from test cases
  $ENV{'VALGRIND_TEST'}= $opt_valgrind;

}


##############################################################################
#
#  If we get a ^C, we try to clean up before termination
#
##############################################################################
# FIXME check restrictions what to do in a signal handler

sub signal_setup () {
  $SIG{INT}= \&handle_int_signal;
}


sub handle_int_signal () {
  $SIG{INT}= 'DEFAULT';         # If we get a ^C again, we die...
  mtr_warning("got INT signal, cleaning up.....");
  stop_all_servers();
  mtr_error("We die from ^C signal from user");
}


##############################################################################
#
#  Handle left overs from previous runs
#
##############################################################################

sub kill_running_servers () {

  if ( $opt_fast or $glob_use_embedded_server )
  {
    # FIXME is embedded server really using PID files?!
    unlink($master->[0]->{'path_pid'});
    unlink($master->[1]->{'path_pid'});
    unlink($slave->[0]->{'path_pid'});
    unlink($slave->[1]->{'path_pid'});
    unlink($slave->[2]->{'path_pid'});
  }
  else
  {
    # Ensure that no old mysqld test servers are running
    # This is different from terminating processes we have
    # started from this run of the script, this is terminating
    # leftovers from previous runs.
    mtr_kill_leftovers();
   }
}

#
# Remove var and any directories in var/ created by previous
# tests
#
sub remove_stale_vardir () {

  mtr_report("Removing Stale Files");

  # Safety!
  mtr_error("No, don't remove the vardir when running with --extern")
    if $opt_extern;

  mtr_verbose("opt_vardir: $opt_vardir");
  if ( $opt_vardir eq $default_vardir )
  {
    #
    # Running with "var" in mysql-test dir
    #
    if ( -l $opt_vardir)
    {
      # var is a symlink

      if ( $opt_mem and readlink($opt_vardir) eq $opt_mem )
      {
	# Remove the directory which the link points at
	mtr_verbose("Removing " . readlink($opt_vardir));
	mtr_rmtree(readlink($opt_vardir));

	# Remove the "var" symlink
	mtr_verbose("unlink($opt_vardir)");
	unlink($opt_vardir);
      }
      elsif ( $opt_mem )
      {
	# Just remove the "var" symlink
	mtr_report("WARNING: Removing '$opt_vardir' symlink it's wrong");

	mtr_verbose("unlink($opt_vardir)");
	unlink($opt_vardir);
      }
      else
      {
	# Some users creates a soft link in mysql-test/var to another area
	# - allow it, but remove all files in it

	mtr_report("WARNING: Using the 'mysql-test/var' symlink");

	# Make sure the directory where it points exist
        if (! -d readlink($opt_vardir))
        {
          mtr_report("The destination for symlink $opt_vardir does not exist; Removing it and creating a new var directory");
          unlink($opt_vardir);
        }

	foreach my $bin ( glob("$opt_vardir/*") )
	{
	  mtr_verbose("Removing bin $bin");
	  mtr_rmtree($bin);
	}
      }
    }
    else
    {
      # Remove the entire "var" dir
      mtr_verbose("Removing $opt_vardir/");
      mtr_rmtree("$opt_vardir/");
    }

    if ( $opt_mem )
    {
      # A symlink from var/ to $opt_mem will be set up
      # remove the $opt_mem dir to assure the symlink
      # won't point at an old directory
      mtr_verbose("Removing $opt_mem");
      mtr_rmtree($opt_mem);
    }

  }
  else
  {
    #
    # Running with "var" in some other place
    #

    # Remove the var/ dir in mysql-test dir if any
    # this could be an old symlink that shouldn't be there
    # mtr_verbose("Removing $default_vardir");
    # mtr_rmtree($default_vardir);

    # Remove the "var" dir
    mtr_verbose("Removing $opt_vardir/");
    mtr_rmtree("$opt_vardir/");
  }
}

#
# Create var and the directories needed in var
#
sub setup_vardir() {
  mtr_report("Creating Directories");

  if ( $opt_vardir eq $default_vardir )
  {
    #
    # Running with "var" in mysql-test dir
    #
    if ( -l $opt_vardir )
    {
      #  it's a symlink

      # Make sure the directory where it points exist
      if (! -d readlink($opt_vardir))
      {
        mtr_report("The destination for symlink $opt_vardir does not exist; Removing it and creating a new var directory");
        unlink($opt_vardir);
      }
    }
    elsif ( $opt_mem )
    {
      # Runinng with "var" as a link to some "memory" location, normally tmpfs
      mtr_verbose("Creating $opt_mem");
      mkpath($opt_mem);

      mtr_report("Symlinking 'var' to '$opt_mem'");
      symlink($opt_mem, $opt_vardir);
    }
  }

  if ( ! -d $opt_vardir )
  {
    mtr_verbose("Creating $opt_vardir");
    mkpath($opt_vardir);
  }

  # Ensure a proper error message if vardir couldn't be created
  unless ( -d $opt_vardir and -w $opt_vardir )
  {
    mtr_error("Writable 'var' directory is needed, use the " .
	      "'--vardir=<path>' option");
  }

  mkpath("$opt_vardir/log");
  mkpath("$opt_vardir/run");
  mkpath("$opt_vardir/tmp");
  mkpath($opt_tmpdir) if $opt_tmpdir ne "$opt_vardir/tmp";

  # Create new data dirs
  foreach my $data_dir (@data_dir_lst)
  {
    mkpath("$data_dir/mysql");
    mkpath("$data_dir/test");
  }

  # Make a link std_data_ln in var/ that points to std_data
  if ( ! $glob_win32 )
  {
    symlink("$glob_mysql_test_dir/std_data", "$opt_vardir/std_data_ln");
  }
  else
  {
    # on windows, copy all files from std_data into var/std_data_ln
    mkpath("$opt_vardir/std_data_ln");
    mtr_copy_dir("$glob_mysql_test_dir/std_data", "$opt_vardir/std_data_ln");
  }

  # Remove old log files
  foreach my $name (glob("r/*.progress r/*.log r/*.warnings"))
  {
    unlink($name);
  }
  if ( $opt_valgrind and $opt_debug )
  {
    # When both --valgrind and --debug is selected, send
    # all output to the trace file, making it possible to
    # see the exact location where valgrind complains
    foreach my $mysqld (@{$master}, @{$slave})
    {
      my $sidx= $mysqld->{idx} ? "$mysqld->{idx}" : "";
      my $trace_name= "$opt_vardir/log/" . $mysqld->{type} . "$sidx.trace";
      open(LOG, ">$mysqld->{path_myerr}") or die "Can't create $mysqld->{path_myerr}\n";
      print LOG "
NOTE: When running with --valgrind --debug the output from the .err file is
stored together with the trace file to make it easier to find the exact
position for valgrind errors.
See trace file $trace_name.\n";
      close(LOG);
      $mysqld->{path_myerr}= $trace_name;
    }
  }
}


sub  check_running_as_root () {
  # Check if running as root
  # i.e a file can be read regardless what mode we set it to
  my $test_file= "$opt_vardir/test_running_as_root.txt";
  mtr_tofile($test_file, "MySQL");
  chmod(oct("0000"), $test_file);

  my $result="";
  if (open(FILE,"<",$test_file))
  {
    $result= join('', <FILE>);
    close FILE;
  }

  # Some filesystems( for example CIFS) allows reading a file
  # although mode was set to 0000, but in that case a stat on
  # the file will not return 0000
  my $file_mode= (stat($test_file))[2] & 07777;

  $ENV{'MYSQL_TEST_ROOT'}= "NO";
  mtr_verbose("result: $result, file_mode: $file_mode");
  if ($result eq "MySQL" && $file_mode == 0)
  {
    mtr_warning("running this script as _root_ will cause some " .
                "tests to be skipped");
    $ENV{'MYSQL_TEST_ROOT'}= "YES";
  }

  chmod(oct("0755"), $test_file);
  unlink($test_file);

}


sub check_ssl_support ($) {
  my $mysqld_variables= shift;

  if ($opt_skip_ssl || $opt_extern)
  {
    if (!$opt_extern)
    {
      mtr_report("Skipping SSL");
    }
    $opt_ssl_supported= 0;
    $opt_ssl= 0;
    return;
  }

  if ( ! $mysqld_variables->{'ssl'} )
  {
    if ( $opt_ssl)
    {
      mtr_error("Couldn't find support for SSL");
      return;
    }
    mtr_report("Skipping SSL, mysqld not compiled with SSL");
    $opt_ssl_supported= 0;
    $opt_ssl= 0;
    return;
  }
  mtr_report("Setting mysqld to support SSL connections");
  $opt_ssl_supported= 1;
}


sub check_debug_support ($) {
  my $mysqld_variables= shift;

  if ( ! $mysqld_variables->{'debug'} )
  {
    #mtr_report("Binaries are not debug compiled");
    $debug_compiled_binaries= 0;

    if ( $opt_debug )
    {
      mtr_error("Can't use --debug, binaries does not support it");
    }
    return;
  }
  mtr_report("Binaries are debug compiled");
  $debug_compiled_binaries= 1;
}

##############################################################################
#
# Helper function to handle configuration-based subdirectories which Visual
# Studio uses for storing binaries.  If opt_vs_config is set, this returns
# a path based on that setting; if not, it returns paths for the default
# /release/ and /debug/ subdirectories.
#
# $exe can be undefined, if the directory itself will be used
#
###############################################################################

sub vs_config_dirs ($$) {
  my ($path_part, $exe) = @_;

  $exe = "" if not defined $exe;

  if ($multiconfig)
  {
    return ("$glob_bindir/$path_part/$multiconfig/$exe");
  }

  return ("$glob_bindir/$path_part/release/$exe",
          "$glob_bindir/$path_part/relwithdebinfo/$exe",
          "$glob_bindir/$path_part/debug/$exe");
}

sub mysqld_wait_started($){
  my $mysqld= shift;

  if (sleep_until_file_created($mysqld->{'path_pid'},
			       $mysqld->{'start_timeout'},
			       $mysqld->{'pid'}) == 0)
  {
    # Failed to wait for pid file
    return 1;
  }

  # Get the "real pid" of the process, it will be used for killing
  # the process in ActiveState's perl on windows
  $mysqld->{'real_pid'}= mtr_get_pid_from_file($mysqld->{'path_pid'});

  return 0;
}


##############################################################################
#
#  Run the benchmark suite
#
##############################################################################

sub run_benchmarks ($) {
  my $benchmark=  shift;

  my $args;

  if ( ! $glob_use_embedded_server )
  {
    mysqld_start($master->[0],[],[]);
    if ( ! $master->[0]->{'pid'} )
    {
      mtr_error("Can't start the mysqld server");
    }
  }

  mtr_init_args(\$args);

  mtr_add_arg($args, "--socket=%s", $master->[0]->{'path_sock'});
  mtr_add_arg($args, "--user=%s", $opt_user);

  if ( $opt_small_bench )
  {
    mtr_add_arg($args, "--small-test");
    mtr_add_arg($args, "--small-tables");
  }

  chdir($glob_mysql_bench_dir)
    or mtr_error("Couldn't chdir to '$glob_mysql_bench_dir': $!");

  if ( ! $benchmark )
  {
    mtr_add_arg($args, "--general-log");
    mtr_run("$glob_mysql_bench_dir/run-all-tests", $args, "", "", "", "");
    # FIXME check result code?!
  }
  elsif ( -x $benchmark )
  {
    mtr_run("$glob_mysql_bench_dir/$benchmark", $args, "", "", "", "");
    # FIXME check result code?!
  }
  else
  {
    mtr_error("Benchmark $benchmark not found");
  }

  chdir($glob_mysql_test_dir);          # Go back

  if ( ! $glob_use_embedded_server )
  {
    stop_masters();
  }
}


##############################################################################
#
#  Run the tests
#
##############################################################################

sub run_tests () {
  my ($tests)= @_;

  mtr_print_thick_line();

  mtr_timer_start($glob_timers,"suite", 60 * $opt_suite_timeout);

  mtr_report_tests_not_skipped_though_disabled($tests);

  mtr_print_header();

  foreach my $tinfo ( @$tests )
  {
    if (run_testcase_check_skip_test($tinfo))
    {
      next;
    }

    mtr_timer_start($glob_timers,"testcase", 60 * $opt_testcase_timeout);
    run_testcase($tinfo);
    mtr_timer_stop($glob_timers,"testcase");
  }

  mtr_print_line();

  if ( ! $glob_debugger and
       ! $opt_extern and
       ! $glob_use_embedded_server )
  {
    stop_all_servers();
  }

  if ( $opt_gcov )
  {
    gcov_collect(); # collect coverage information
  }
  if ( $opt_gprof )
  {
    gprof_collect(); # collect coverage information
  }

  mtr_report_stats($tests);

  mtr_timer_stop($glob_timers,"suite");
}


##############################################################################
#
#  Initiate the test databases
#
##############################################################################

sub initialize_servers () {

  datadir_list_setup();

  if ( $opt_extern )
  {
    # Running against an already started server, if the specified
    # vardir does not already exist it should be created
    if ( ! -d $opt_vardir )
    {
      mtr_report("Creating '$opt_vardir'");
      setup_vardir();
    }
    else
    {
      mtr_verbose("No need to create '$opt_vardir' it already exists");
    }
  }
  else
  {
    kill_running_servers();

    if ( ! $opt_start_dirty )
    {
      remove_stale_vardir();
      setup_vardir();

      mysql_install_db();
      if ( $opt_force )
      {
	# Save a snapshot of the freshly installed db
	# to make it possible to restore to a known point in time
	save_installed_db();
      }
    }
  }
  check_running_as_root();

  mtr_log_init("$opt_vardir/log/mysql-test-run.log");

}

sub mysql_install_db () {

  install_db('master', $master->[0]->{'path_myddir'});

  if ($max_master_num > 1)
  {
    copy_install_db('master', $master->[1]->{'path_myddir'});
  }

  # Install the number of slave databses needed
  for (my $idx= 0; $idx < $max_slave_num; $idx++)
  {
    copy_install_db("slave".($idx+1), $slave->[$idx]->{'path_myddir'});
  }

  return 0;
}


sub copy_install_db ($$) {
  my $type=      shift;
  my $data_dir=  shift;

  mtr_report("Installing \u$type Database");

  # Just copy the installed db from first master
  mtr_copy_dir($master->[0]->{'path_myddir'}, $data_dir);

}


sub install_db ($$) {
  my $type=      shift;
  my $data_dir=  shift;

  mtr_report("Installing \u$type Database");

  my $args;
  my $cmd_args;
  mtr_init_args(\$args);
  mtr_add_arg($args, "--no-defaults");
  mtr_add_arg($args, "--bootstrap");
  mtr_add_arg($args, "--basedir=%s", $glob_basedir);
  mtr_add_arg($args, "--datadir=%s", $data_dir);
  mtr_add_arg($args, "--loose-skip-aria");
  mtr_add_arg($args, "--disable-sync-frm");
  mtr_add_arg($args, "--loose-disable-debug");
  mtr_add_arg($args, "--tmpdir=.");
  mtr_add_arg($args, "--core-file");

  #
  # Setup args for bootstrap.test
  #
  mtr_init_args(\$cmd_args);
  mtr_add_arg($cmd_args, "--loose-skip-aria");

  if ( $opt_debug )
  {
    mtr_add_arg($args, "--debug=d:t:i:A,%s/log/bootstrap_%s.trace",
		$path_vardir_trace, $type);
  }

  mtr_add_arg($args, "--lc-messages-dir=%s", $path_language);
  mtr_add_arg($args, "--character-sets-dir=%s", $path_charsetsdir);

  # InnoDB arguments that affect file location and sizes may
  # need to be given to the bootstrap process as well as the
  # server process.
  foreach my $extra_opt ( @opt_extra_mysqld_opt ) {
    if ($extra_opt =~ /--innodb/) {
      mtr_add_arg($args, $extra_opt);
    }
  }

  # If DISABLE_GRANT_OPTIONS is defined when the server is compiled (e.g.,
  # configure --disable-grant-options), mysqld will not recognize the
  # --bootstrap or --skip-grant-tables options.  The user can set
  # MYSQLD_BOOTSTRAP to the full path to a mysqld which does accept
  # --bootstrap, to accommodate this.
  my $exe_mysqld_bootstrap = $ENV{'MYSQLD_BOOTSTRAP'} || $exe_mysqld;

  # ----------------------------------------------------------------------
  # export MYSQLD_BOOTSTRAP_CMD variable containing <path>/mysqld <args>
  # ----------------------------------------------------------------------
  $ENV{'MYSQLD_BOOTSTRAP_CMD'}= "$exe_mysqld_bootstrap " . join(" ", @$args) .
    " " . join(" ", @$cmd_args);

  # ----------------------------------------------------------------------
  # Create the bootstrap.sql file
  # ----------------------------------------------------------------------
  my $bootstrap_sql_file= "$opt_vardir/tmp/bootstrap.sql";

  # Use the mysql database for system tables
  mtr_tofile($bootstrap_sql_file, "use mysql;\n");

  # Add the offical mysql system tables
  # for a production system
  mtr_appendfile_to_file("$path_sql_dir/mysql_system_tables.sql",
			 $bootstrap_sql_file);
  mtr_appendfile_to_file("$path_sql_dir/mysql_performance_tables.sql",
			 $bootstrap_sql_file);

  # Add the mysql system tables initial data
  # for a production system
  mtr_appendfile_to_file("$path_sql_dir/mysql_system_tables_data.sql",
			 $bootstrap_sql_file);

  # Add test data for timezone - this is just a subset, on a real
  # system these tables will be populated either by mysql_tzinfo_to_sql
  # or by downloading the timezone table package from our website
  mtr_appendfile_to_file("$path_sql_dir/mysql_test_data_timezone.sql",
			 $bootstrap_sql_file);

  # Fill help tables, just an empty file when running from bk repo
  # but will be replaced by a real fill_help_tables.sql when
  # building the source dist
  mtr_appendfile_to_file("$path_sql_dir/fill_help_tables.sql",
			 $bootstrap_sql_file);

  # Remove anonymous users
  mtr_tofile($bootstrap_sql_file,
	     "DELETE FROM mysql.user where user= '';");

  # Log bootstrap command
  my $path_bootstrap_log= "$opt_vardir/log/bootstrap.log";
  mtr_tofile($path_bootstrap_log,
	     "$exe_mysqld_bootstrap " . join(" ", @$args) . "\n");


  if ( mtr_run($exe_mysqld_bootstrap, $args, $bootstrap_sql_file,
               $path_bootstrap_log, $path_bootstrap_log,
	       "", { append_log_file => 1 }) != 0 )

  {
    mtr_error("Error executing mysqld --bootstrap\n" .
              "Could not install system database from $bootstrap_sql_file\n" .
	      "see $path_bootstrap_log for errors");
  }
}


#
# Restore snapshot of the installed slave databases
# if the snapshot exists
#
sub restore_slave_databases ($) {
  my ($num_slaves)= @_;

  if ( -d $path_snapshot)
  {
    for (my $idx= 0; $idx < $num_slaves; $idx++)
    {
      my $data_dir= $slave->[$idx]->{'path_myddir'};
      my $name= basename($data_dir);
      mtr_rmtree($data_dir);
      mtr_copy_dir("$path_snapshot/$name", $data_dir);
    }
  }
}


sub run_testcase_check_skip_test($)
{
  my ($tinfo)= @_;

  # ----------------------------------------------------------------------
  # Skip some tests silently
  # ----------------------------------------------------------------------

  if ( $::opt_start_from )
  {
    if ($tinfo->{'name'} eq $::opt_start_from )
    {
      ## Found parting test. Run this test and all tests after this one
      $::opt_start_from= "";
    }
    else
    {
      $tinfo->{'result'}= 'MTR_RES_SKIPPED';
      return 1;
    }
  }

  # ----------------------------------------------------------------------
  # If marked to skip, just print out and return.
  # Note that a test case not marked as 'skip' can still be
  # skipped later, because of the test case itself in cooperation
  # with the mysqltest program tells us so.
  # ----------------------------------------------------------------------

  if ( $tinfo->{'skip'} )
  {
    mtr_report_test_name($tinfo);
    mtr_report_test_skipped($tinfo);
    return 1;
  }

  return 0;
}


sub do_before_run_mysqltest($)
{
  my $tinfo= shift;
  my $args;

  # Remove old files produced by mysqltest
  my $base_file= mtr_match_extension($tinfo->{'result_file'},
				    "result"); # Trim extension
  unlink("$base_file.reject");
  unlink("$base_file.progress");
  unlink("$base_file.log");
  unlink("$base_file.warnings");

  if (!$opt_extern)
  {
    if (defined $tinfo->{binlog_format} and  $mysql_version_id > 50100 )
    {
      # Dynamically switch binlog format of
      # master, slave is always restarted
      foreach my $server ( @$master )
      {
        next unless ($server->{'pid'});

	mtr_init_args(\$args);
	mtr_add_arg($args, "--no-defaults");
	mtr_add_arg($args, "--user=root");
	mtr_add_arg($args, "--port=$server->{'port'}");
	mtr_add_arg($args, "--socket=$server->{'path_sock'}");

	my $sql= "include/set_binlog_format_".$tinfo->{binlog_format}.".sql";
	mtr_verbose("Setting binlog format:", $tinfo->{binlog_format});
	if (mtr_run($exe_mysql, $args, $sql, "", "", "") != 0)
	{
	  mtr_error("Failed to switch binlog format");
	}
      }
    }
  }
}

sub do_after_run_mysqltest($)
{
  my $tinfo= shift;

  # Save info from this testcase run to mysqltest.log
  mtr_appendfile_to_file($path_current_test_log, $path_mysqltest_log)
    if -f $path_current_test_log;
  mtr_appendfile_to_file($path_timefile, $path_mysqltest_log)
    if -f $path_timefile;
}


sub run_testcase_mark_logs($$)
{
  my ($tinfo, $log_msg)= @_;

  # Write a marker to all log files

  # The file indicating current test name
  mtr_tonewfile($path_current_test_log, $log_msg);

  # each mysqld's .err file
  foreach my $mysqld (@{$master}, @{$slave})
  {
    mtr_tofile($mysqld->{path_myerr}, $log_msg);
  }
}

sub find_testcase_skipped_reason($)
{
  my ($tinfo)= @_;

  # Set default message
  $tinfo->{'comment'}= "Detected by testcase(no log file)";

  # Open mysqltest-time(the mysqltest log file)
  my $F= IO::File->new($path_timefile)
    or return;
  my $reason;

  while ( my $line= <$F> )
  {
    # Look for "reason: <reason for skipping test>"
    if ( $line =~ /reason: (.*)/ )
    {
      $reason= $1;
    }
  }

  if ( ! $reason )
  {
    mtr_warning("Could not find reason for skipping test in $path_timefile");
    $reason= "Detected by testcase(reason unknown) ";
  }
  $tinfo->{'comment'}= $reason;
}


##############################################################################
#
#  Run a single test case
#
##############################################################################

# When we get here, we have already filtered out test cases that doesn't
# apply to the current setup, for example if we use a running server, test
# cases that restart the server are dropped. So this function should mostly
# be about doing things, not a lot of logic.

# We don't start and kill the servers for each testcase. But some
# testcases needs a restart, because they specify options to start
# mysqld with. After that testcase, we need to restart again, to set
# back the normal options.

sub run_testcase ($) {
  my $tinfo=  shift;

  # -------------------------------------------------------
  # Init variables that can change between each test case
  # -------------------------------------------------------

  $ENV{'TZ'}= $tinfo->{'timezone'};
  mtr_verbose("Setting timezone: $tinfo->{'timezone'}");

  my $master_restart= run_testcase_need_master_restart($tinfo);
  my $slave_restart= run_testcase_need_slave_restart($tinfo);

  if ($master_restart or $slave_restart)
  {
    # Can't restart a running server that may be in use
    if ( $opt_extern )
    {
      mtr_report_test_name($tinfo);
      $tinfo->{comment}= "Can't restart a running server";
      mtr_report_test_skipped($tinfo);
      return;
    }

    run_testcase_stop_servers($tinfo, $master_restart, $slave_restart);
  }

  # Write to all log files to indicate start of testcase
  run_testcase_mark_logs($tinfo, "CURRENT_TEST: $tinfo->{name}\n");

  my $died= mtr_record_dead_children();
  if ($died or $master_restart or $slave_restart)
  {
    if (run_testcase_start_servers($tinfo))
    {
      mtr_report_test_name($tinfo);
      report_failure_and_restart($tinfo);
      return 1;
    }
  }
  elsif ($glob_use_embedded_server)
  {
    run_master_init_script($tinfo);
  }

  # ----------------------------------------------------------------------
  # If --start-and-exit or --start-dirty given, stop here to let user manually
  # run tests
  # ----------------------------------------------------------------------
  if ( $opt_start_and_exit or $opt_start_dirty )
  {
    mtr_timer_stop_all($glob_timers);
    mtr_report("\nServers started, exiting");
    if ($glob_win32_perl)
    {
      #ActiveState perl hangs  when using normal exit, use  POSIX::_exit instead
      use POSIX qw[ _exit ];
      POSIX::_exit(0);
    }
    else
    {
      exit(0);
    }
  }

  {
    do_before_run_mysqltest($tinfo);

    my $res= run_mysqltest($tinfo);
    mtr_report_test_name($tinfo);

    do_after_run_mysqltest($tinfo);

    if ( $res == 0 )
    {
      mtr_report_test_passed($tinfo);
    }
    elsif ( $res == 62 )
    {
      # Testcase itself tell us to skip this one

      # Try to get reason from mysqltest.log
      find_testcase_skipped_reason($tinfo);
      mtr_report_test_skipped($tinfo);
    }
    elsif ( $res == 63 )
    {
      $tinfo->{'timeout'}= 1;           # Mark as timeout
      report_failure_and_restart($tinfo);
    }
    elsif ( $res == 1 )
    {
      # Test case failure reported by mysqltest
      report_failure_and_restart($tinfo);
    }
    else
    {
      # mysqltest failed, probably crashed
      $tinfo->{comment}=
	"mysqltest returned unexpected code $res, it has probably crashed";
      report_failure_and_restart($tinfo);
    }
  }

  # Remove the file that mysqltest writes info to
  unlink($path_timefile);
}


#
# Save a snapshot of the installed test db(s)
# I.e take a snapshot of the var/ dir
#
sub save_installed_db () {

  mtr_report("Saving snapshot of installed databases");
  mtr_rmtree($path_snapshot);

  foreach my $data_dir (@data_dir_lst)
  {
    my $name= basename($data_dir);
    mtr_copy_dir("$data_dir", "$path_snapshot/$name");
  }
}


#
# Save any interesting files in the data_dir
# before the data dir is removed.
#
sub save_files_before_restore($$) {
  my $test_name= shift;
  my $data_dir= shift;
  my $save_name= "$opt_vardir/log/$test_name";

  # Look for core files
  foreach my $core_file ( glob("$data_dir/core*") )
  {
    last if $opt_max_save_core > 0 && $num_saved_cores >= $opt_max_save_core;
    my $core_name= basename($core_file);
    mtr_report("Saving $core_name");
    mkdir($save_name) if ! -d $save_name;
    rename("$core_file", "$save_name/$core_name");
    ++$num_saved_cores;
  }
}


#
# Restore snapshot of the installed test db(s)
# if the snapshot exists
#
sub restore_installed_db ($) {
  my $test_name= shift;

  if ( -d $path_snapshot)
  {
    mtr_report("Restoring snapshot of databases");

    foreach my $data_dir (@data_dir_lst)
    {
      my $name= basename($data_dir);
      save_files_before_restore($test_name, $data_dir);
      mtr_rmtree("$data_dir");
      mtr_copy_dir("$path_snapshot/$name", "$data_dir");
    }
  }
  else
  {
    # No snapshot existed
    mtr_error("No snapshot existed");
  }
}

sub report_failure_and_restart ($) {
  my $tinfo= shift;

  mtr_report_test_failed($tinfo);
  print "\n";
  if ( $opt_force )
  {
    # Stop all servers that are known to be running
    stop_all_servers();

    # Restore the snapshot of the installed test db
    restore_installed_db($tinfo->{'name'});
    mtr_report("Resuming Tests\n");
    return;
  }

  my $test_mode= join(" ", @::glob_test_mode) || "default";
  mtr_report("Aborting: $tinfo->{'name'} failed in $test_mode mode. ");
  mtr_report("To continue, re-run with '--force'.");
  if ( ! $glob_debugger and
       ! $opt_extern and
       ! $glob_use_embedded_server )
  {
    stop_all_servers();
  }
  mtr_exit(1);

}


sub run_master_init_script ($) {
  my ($tinfo)= @_;
  my $init_script= $tinfo->{'master_sh'};

  # Run master initialization shell script if one exists
  if ( $init_script )
  {
    my $ret= mtr_run("/bin/sh", [$init_script], "", "", "", "");
    if ( $ret != 0 )
    {
      # FIXME rewrite those scripts to return 0 if successful
      # mtr_warning("$init_script exited with code $ret");
    }
  }
}


##############################################################################
#
#  Start and stop servers
#
##############################################################################


sub do_before_start_master ($) {
  my ($tinfo)= @_;

  my $tname= $tinfo->{'name'};

  # FIXME what about second master.....

  # Don't delete anything if starting dirty
  return if ($opt_start_dirty);

  foreach my $bin ( glob("$opt_vardir/log/master*-bin*") )
  {
    unlink($bin);
  }

  # FIXME only remove the ones that are tied to this master
  # Remove old master.info and relay-log.info files
  unlink("$master->[0]->{'path_myddir'}/master.info");
  unlink("$master->[0]->{'path_myddir'}/relay-log.info");
  unlink("$master->[1]->{'path_myddir'}/master.info");
  unlink("$master->[1]->{'path_myddir'}/relay-log.info");

  run_master_init_script($tinfo);
}


sub do_before_start_slave ($) {
  my ($tinfo)= @_;

  my $tname= $tinfo->{'name'};
  my $init_script= $tinfo->{'master_sh'};

  # Don't delete anything if starting dirty
  return if ($opt_start_dirty);

  foreach my $bin ( glob("$opt_vardir/log/slave*-bin*") )
  {
    unlink($bin);
  }

  unlink("$slave->[0]->{'path_myddir'}/master.info");
  unlink("$slave->[0]->{'path_myddir'}/relay-log.info");

  # Run slave initialization shell script if one exists
  if ( $init_script )
  {
    my $ret= mtr_run("/bin/sh", [$init_script], "", "", "", "");
    if ( $ret != 0 )
    {
      # FIXME rewrite those scripts to return 0 if successful
      # mtr_warning("$init_script exited with code $ret");
    }
  }

  foreach my $bin ( glob("$slave->[0]->{'path_myddir'}/log.*") )
  {
    unlink($bin);
  }
}


sub mysqld_arguments ($$$$) {
  my $args=              shift;
  my $mysqld=            shift;
  my $extra_opt=         shift;
  my $slave_master_info= shift;

  my $idx= $mysqld->{'idx'};
  my $sidx= "";                 # Index as string, 0 is empty string
  if ( $idx> 0 )
  {
    $sidx= $idx;
  }

  my $prefix= "";               # If mysqltest server arg
  if ( $glob_use_embedded_server )
  {
    $prefix= "--server-arg=";
  }

  mtr_add_arg($args, "%s--no-defaults", $prefix);

  mtr_add_arg($args, "%s--basedir=%s", $prefix, $glob_basedir);
  mtr_add_arg($args, "%s--character-sets-dir=%s", $prefix, $path_charsetsdir);

  if ( $mysql_version_id >= 50036)
  {
    # By default, prevent the started mysqld to access files outside of vardir
    mtr_add_arg($args, "%s--secure-file-priv=%s", $prefix, $opt_vardir);
  }

  if ( $mysql_version_id >= 50000 )
  {
    mtr_add_arg($args, "%s--log-bin-trust-function-creators", $prefix);
  }

  mtr_add_arg($args, "%s--character-set-server=latin1", $prefix);
  mtr_add_arg($args, "%s--lc-messages-dir=%s", $prefix, $path_language);
  mtr_add_arg($args, "%s--tmpdir=$opt_tmpdir", $prefix);

  # Increase default connect_timeout to avoid intermittent
  # disconnects when test servers are put under load
  # see BUG#28359
  mtr_add_arg($args, "%s--connect-timeout=60", $prefix);


  # When mysqld is run by a root user(euid is 0), it will fail
  # to start unless we specify what user to run as, see BUG#30630
  my $euid= $>;
  if (!$glob_win32 and $euid == 0 and
      grep(/^--user/, @$extra_opt, @opt_extra_mysqld_opt) == 0) {
    mtr_add_arg($args, "%s--user=root", $prefix);
  }

  if ( $opt_valgrind_mysqld )
  {
    if ( $mysql_version_id < 50100 )
    {
      mtr_add_arg($args, "%s--skip-bdb", $prefix);
    }
  }

  mtr_add_arg($args, "%s--pid-file=%s", $prefix,
	      $mysqld->{'path_pid'});

  mtr_add_arg($args, "%s--port=%d", $prefix,
                $mysqld->{'port'});

  mtr_add_arg($args, "%s--socket=%s", $prefix,
	      $mysqld->{'path_sock'});

  mtr_add_arg($args, "%s--datadir=%s", $prefix,
	      $mysqld->{'path_myddir'});

  mtr_add_arg($args, "%s--disable-sync-frm", $prefix);  # Faster test

  if (!$opt_extern and $mysql_version_id >= 50106 )
  {
    # Turn on logging to bothe tables and file
    mtr_add_arg($args, "%s--log-output=table,file", $prefix);
  }

  my $log_base_path= "$opt_vardir/log/$mysqld->{'type'}$sidx";
  mtr_add_arg($args, "%s--general-log-file=%s.log",
              $prefix, $log_base_path);
  mtr_add_arg($args, "%s--general-log", $prefix);
  mtr_add_arg($args,
	      "%s--slow-query-log-file=%s-slow.log",
              $prefix, $log_base_path);
  mtr_add_arg($args, "%s--slow-query-log", $prefix);

  # Check if "extra_opt" contains --skip-log-bin
  my $skip_binlog= grep(/^--skip-log-bin/, @$extra_opt, @opt_extra_mysqld_opt);
  if ( $mysqld->{'type'} eq 'master' )
  {
    if (! ($opt_skip_master_binlog || $skip_binlog) )
    {
      mtr_add_arg($args, "%s--log-bin=%s/log/master-bin%s", $prefix,
                  $opt_vardir, $sidx);
    }

    mtr_add_arg($args, "%s--server-id=%d", $prefix,
	       $idx > 0 ? $idx + 101 : 1);

    mtr_add_arg($args, "%s--loose-innodb_data_file_path=ibdata1:10M:autoextend",
		$prefix);

    mtr_add_arg($args, "%s--local-infile", $prefix);
  }
  else
  {
    mtr_error("unknown mysqld type")
      unless $mysqld->{'type'} eq 'slave';

    mtr_add_arg($args, "%s--init-rpl-role=slave", $prefix);
    if (! ( $opt_skip_slave_binlog || $skip_binlog ))
    {
      mtr_add_arg($args, "%s--log-bin=%s/log/slave%s-bin", $prefix,
                  $opt_vardir, $sidx); # FIXME use own dir for binlogs
      mtr_add_arg($args, "%s--log-slave-updates", $prefix);
    }

    mtr_add_arg($args, "%s--master-retry-count=10", $prefix);

    mtr_add_arg($args, "%s--relay-log=%s/log/slave%s-relay-bin", $prefix,
                $opt_vardir, $sidx);
    mtr_add_arg($args, "%s--report-host=127.0.0.1", $prefix);
    mtr_add_arg($args, "%s--report-port=%d", $prefix,
                $mysqld->{'port'});
    mtr_add_arg($args, "%s--report-user=root", $prefix);
    mtr_add_arg($args, "%s--loose-skip-innodb", $prefix);
    mtr_add_arg($args, "%s--skip-slave-start", $prefix);

    # Directory where slaves find the dumps generated by "load data"
    # on the server. The path need to have constant length otherwise
    # test results will vary, thus a relative path is used.
    my $slave_load_path= "../tmp";
    mtr_add_arg($args, "%s--slave-load-tmpdir=%s", $prefix,
                $slave_load_path);
    mtr_add_arg($args, "%s--slave_net_timeout=120", $prefix);

    if ( @$slave_master_info )
    {
      foreach my $arg ( @$slave_master_info )
      {
        mtr_add_arg($args, "%s%s", $prefix, $arg);
      }
    }
    else
    {
#      NOTE: the backport (see BUG#48048) originally removed the
#            commented out lines below. However, given that they are
#            protected with a version check (< 50200) now, it should be
#            safe to keep them. The problem is that the backported patch
#            was into a 5.1 GA codebase - mysql-5.1-rep+2 tree - so
#            version is 501XX, consequently check becomes worthless. It
#            should be safe to uncomment them when merging up to 5.5.
#
#            RQG semisync test runs on the 5.1 GA tree and needs MTR v1.
#            This was causing the test to fail (slave would not start
#            due to unrecognized option(s)).
#      if ($mysql_version_id < 50200)
#      {
#        mtr_add_arg($args, "%s--master-user=root", $prefix);
#        mtr_add_arg($args, "%s--master-connect-retry=1", $prefix);
#        mtr_add_arg($args, "%s--master-host=127.0.0.1", $prefix);
#        mtr_add_arg($args, "%s--master-password=", $prefix);
#        mtr_add_arg($args, "%s--master-port=%d", $prefix,
#    	            $master->[0]->{'port'}); # First master
#      }
      my $slave_server_id=  2 + $idx;
      my $slave_rpl_rank= $slave_server_id;
      mtr_add_arg($args, "%s--server-id=%d", $prefix, $slave_server_id);
      mtr_add_arg($args, "%s--rpl-recovery-rank=%d", $prefix, $slave_rpl_rank);
    }
  } # end slave

  if ( $debug_compiled_binaries && defined $opt_debug )
  {
    if ( $opt_debug )
    {
      mtr_add_arg($args, "%s--debug=d:t:i:A,%s/log/%s%s.trace",
                  $prefix, $path_vardir_trace, $mysqld->{'type'}, $sidx);
    }
    else
    {
      mtr_add_arg($args, "--disable-debug");
    }
  }

  mtr_add_arg($args, "%s--key_buffer_size=1M", $prefix);
  mtr_add_arg($args, "%s--sort_buffer_size=256K", $prefix);
  mtr_add_arg($args, "%s--max_heap_table_size=1M", $prefix);

  if ( $opt_ssl_supported )
  {
    mtr_add_arg($args, "%s--ssl-ca=%s/std_data/cacert.pem", $prefix,
                $glob_mysql_test_dir);
    mtr_add_arg($args, "%s--ssl-cert=%s/std_data/server-cert.pem", $prefix,
                $glob_mysql_test_dir);
    mtr_add_arg($args, "%s--ssl-key=%s/std_data/server-key.pem", $prefix,
                $glob_mysql_test_dir);
  }

  if ( $opt_warnings )
  {
    mtr_add_arg($args, "%s--log-warnings", $prefix);
  }

  # Indicate to "mysqld" it will be debugged in debugger
  if ( $glob_debugger )
  {
    mtr_add_arg($args, "%s--gdb", $prefix);
  }

  my $found_skip_core= 0;
  foreach my $arg ( @opt_extra_mysqld_opt, @$extra_opt )
  {
    # Allow --skip-core-file to be set in <testname>-[master|slave].opt file
    if ($arg eq "--skip-core-file")
    {
      $found_skip_core= 1;
    }
    elsif ($skip_binlog and mtr_match_prefix($arg, "--binlog-format"))
    {
      ; # Dont add --binlog-format when running without binlog
    }
    else
    {
      mtr_add_arg($args, "%s%s", $prefix, $arg);
    }
  }
  if ( !$found_skip_core )
  {
    mtr_add_arg($args, "%s%s", $prefix, "--core-file");
  }

  if ( $opt_bench )
  {
    mtr_add_arg($args, "%s--rpl-recovery-rank=1", $prefix);
    mtr_add_arg($args, "%s--init-rpl-role=master", $prefix);
  }
  elsif ( $mysqld->{'type'} eq 'master' )
  {
    mtr_add_arg($args, "%s--open-files-limit=1024", $prefix);
  }

  return $args;
}


##############################################################################
#
#  Start mysqld and return the PID
#
##############################################################################

sub mysqld_start ($$$) {
  my $mysqld=            shift;
  my $extra_opt=         shift;
  my $slave_master_info= shift;

  my $args;                             # Arg vector
  my $exe;
  my $pid= -1;
  my $wait_for_pid_file= 1;

  my $type= $mysqld->{'type'};
  my $idx= $mysqld->{'idx'};

  mtr_error("Internal error: mysqld should never be started for embedded")
    if $glob_use_embedded_server;

  if ( $type eq 'master' )
  {
    $exe= $exe_master_mysqld;
  }
  elsif ( $type eq 'slave' )
  {
    $exe= $exe_slave_mysqld;
  }
  else
  {
    mtr_error("Unknown 'type' \"$type\" passed to mysqld_start");
  }

  mtr_init_args(\$args);

  if ( $opt_valgrind_mysqld )
  {
    valgrind_arguments($args, \$exe);
  }

  mysqld_arguments($args,$mysqld,$extra_opt,$slave_master_info);

  if ( $opt_gdb || $opt_manual_gdb)
  {
    gdb_arguments(\$args, \$exe, "$type"."_$idx");
  }
  elsif ( $opt_ddd || $opt_manual_ddd )
  {
    ddd_arguments(\$args, \$exe, "$type"."_$idx");
  }
  elsif ( $opt_debugger )
  {
    debugger_arguments(\$args, \$exe, "$type"."_$idx");
  }
  elsif ( $opt_manual_debug )
  {
     print "\nStart $type in your debugger\n" .
           "dir: $glob_mysql_test_dir\n" .
           "exe: $exe\n" .
	   "args:  " . join(" ", @$args)  . "\n\n" .
	   "Waiting ....\n";

     # Indicate the exe should not be started
    $exe= undef;
  }
  else
  {
    # Default to not wait until pid file has been created
    $wait_for_pid_file= 0;
  }

  # Remove the pidfile
  unlink($mysqld->{'path_pid'});

  if ( defined $exe )
  {
    $pid= mtr_spawn($exe, $args, "",
		    $mysqld->{'path_myerr'},
		    $mysqld->{'path_myerr'},
		    "",
		    { append_log_file => 1 });
  }


  if ( $wait_for_pid_file && !sleep_until_file_created($mysqld->{'path_pid'},
						       $mysqld->{'start_timeout'},
						       $pid))
  {

    mtr_error("Failed to start mysqld $mysqld->{'type'}");
  }


  # Remember pid of the started process
  $mysqld->{'pid'}= $pid;

  # Remember options used when starting
  $mysqld->{'start_opts'}= $extra_opt;
  $mysqld->{'start_slave_master_info'}= $slave_master_info;

  mtr_verbose("mysqld pid: $pid");
  return $pid;
}


sub stop_all_servers () {

  mtr_report("Stopping All Servers");

  my %admin_pids; # hash of admin processes that requests shutdown
  my @kill_pids;  # list of processes to shutdown/kill
  my $pid;

  # Start shutdown of all started masters
  foreach my $mysqld (@{$slave}, @{$master})
  {
    if ( $mysqld->{'pid'} )
    {
      $pid= mtr_mysqladmin_start($mysqld, "shutdown", 70);
      $admin_pids{$pid}= 1;

      push(@kill_pids,{
		       pid      => $mysqld->{'pid'},
                       real_pid => $mysqld->{'real_pid'},
		       pidfile  => $mysqld->{'path_pid'},
		       sockfile => $mysqld->{'path_sock'},
		       port     => $mysqld->{'port'},
                       errfile  => $mysqld->{'path_myerr'},
		      });

      $mysqld->{'pid'}= 0; # Assume we are done with it
    }
  }

  # Wait blocking until all shutdown processes has completed
  mtr_wait_blocking(\%admin_pids);

  # Make sure that process has shutdown else try to kill them
  mtr_check_stop_servers(\@kill_pids);
}


sub run_testcase_need_master_restart($)
{
  my ($tinfo)= @_;

  # We try to find out if we are to restart the master(s)
  my $do_restart= 0;          # Assumes we don't have to

  if ( $glob_use_embedded_server )
  {
    mtr_verbose("Never start or restart for embedded server");
    return $do_restart;
  }
  elsif ( $tinfo->{'master_sh'} )
  {
    $do_restart= 1;           # Always restart if script to run
    mtr_verbose("Restart master: Always restart if script to run");
  }
  if ( $tinfo->{'force_restart'} )
  {
    $do_restart= 1; # Always restart if --force-restart in -opt file
    mtr_verbose("Restart master: Restart forced with --force-restart");
  }
  elsif ( $master->[0]->{'running_master_options'} and
	  $master->[0]->{'running_master_options'}->{'timezone'} ne
	  $tinfo->{'timezone'})
  {
    $do_restart= 1;
    mtr_verbose("Restart master: Different timezone");
  }
  # Check that running master was started with same options
  # as the current test requires
  elsif (! mtr_same_opts($master->[0]->{'start_opts'},
                         $tinfo->{'master_opt'}) )
  {
    $do_restart= 1;
    mtr_verbose("Restart master: running with different options '" .
		join(" ", @{$tinfo->{'master_opt'}}) . "' != '" .
	  	join(" ", @{$master->[0]->{'start_opts'}}) . "'" );
  }
  elsif( ! $master->[0]->{'pid'} )
  {
    if ( $opt_extern )
    {
      $do_restart= 0;
      mtr_verbose("No restart: using extern master");
    }
    else
    {
      $do_restart= 1;
      mtr_verbose("Restart master: master is not started");
    }
  }
  return $do_restart;
}

sub run_testcase_need_slave_restart($)
{
  my ($tinfo)= @_;

  # We try to find out if we are to restart the slaves
  my $do_slave_restart= 0;     # Assumes we don't have to

  if ( $glob_use_embedded_server )
  {
    mtr_verbose("Never start or restart for embedded server");
    return $do_slave_restart;
  }
  elsif ( $max_slave_num == 0)
  {
    mtr_verbose("Skip slave restart: No testcase use slaves");
  }
  else
  {

    # Check if any slave is currently started
    my $any_slave_started= 0;
    foreach my $mysqld (@{$slave})
    {
      if ( $mysqld->{'pid'} )
      {
	$any_slave_started= 1;
	last;
      }
    }

    if ($any_slave_started)
    {
      mtr_verbose("Restart slave: Slave is started, always restart");
      $do_slave_restart= 1;
    }
    elsif ( $tinfo->{'slave_num'} )
    {
      mtr_verbose("Restart slave: Test need slave");
      $do_slave_restart= 1;
    }
  }

  return $do_slave_restart;

}

# ----------------------------------------------------------------------
# If not using a running servers we may need to stop and restart.
# We restart in the case we have initiation scripts, server options
# etc to run. But we also restart again after the test first restart
# and test is run, to get back to normal server settings.
#
# To make the code a bit more clean, we actually only stop servers
# here, and mark this to be done. Then a generic "start" part will
# start up the needed servers again.
# ----------------------------------------------------------------------

sub run_testcase_stop_servers($$$) {
  my ($tinfo, $do_restart, $do_slave_restart)= @_;
  my $pid;
  my %admin_pids; # hash of admin processes that requests shutdown
  my @kill_pids;  # list of processes to shutdown/kill

  # Remember if we restarted for this test case (count restarts)
  $tinfo->{'restarted'}= $do_restart;

  if ( $do_restart )
  {
    delete $master->[0]->{'running_master_options'}; # Forget history

    # Start shutdown of all started masters
    foreach my $mysqld (@{$master})
    {
      if ( $mysqld->{'pid'} )
      {
	$pid= mtr_mysqladmin_start($mysqld, "shutdown", 20);

	$admin_pids{$pid}= 1;

	push(@kill_pids,{
			 pid      => $mysqld->{'pid'},
			 real_pid => $mysqld->{'real_pid'},
			 pidfile  => $mysqld->{'path_pid'},
			 sockfile => $mysqld->{'path_sock'},
			 port     => $mysqld->{'port'},
			 errfile   => $mysqld->{'path_myerr'},
			});

	$mysqld->{'pid'}= 0; # Assume we are done with it
      }
    }
  }

  if ( $do_restart || $do_slave_restart )
  {

    delete $slave->[0]->{'running_slave_options'}; # Forget history

    # Start shutdown of all started slaves
    foreach my $mysqld (@{$slave})
    {
      if ( $mysqld->{'pid'} )
      {
	$pid= mtr_mysqladmin_start($mysqld, "shutdown", 20);

	$admin_pids{$pid}= 1;

	push(@kill_pids,{
			 pid      => $mysqld->{'pid'},
			 real_pid => $mysqld->{'real_pid'},
			 pidfile  => $mysqld->{'path_pid'},
			 sockfile => $mysqld->{'path_sock'},
			 port     => $mysqld->{'port'},
			 errfile   => $mysqld->{'path_myerr'},
			});


	$mysqld->{'pid'}= 0; # Assume we are done with it
      }
    }
  }

  # ----------------------------------------------------------------------
  # Shutdown has now been started and lists for the shutdown processes
  # and the processes to be killed has been created
  # ----------------------------------------------------------------------

  # Wait blocking until all shutdown processes has completed
  mtr_wait_blocking(\%admin_pids);


  # Make sure that process has shutdown else try to kill them
  mtr_check_stop_servers(\@kill_pids);
}


#
# run_testcase_start_servers
#
# Start the servers needed by this test case
#
# RETURN
#  0 OK
#  1 Start failed
#

sub run_testcase_start_servers($) {
  my $tinfo= shift;
  my $tname= $tinfo->{'name'};

  if ( $tinfo->{'component_id'} eq 'mysqld' )
  {
    if ( !$master->[0]->{'pid'} )
    {
      # Master mysqld is not started
      do_before_start_master($tinfo);

      mysqld_start($master->[0],$tinfo->{'master_opt'},[]);

    }

    # Save this test case information, so next can examine it
    $master->[0]->{'running_master_options'}= $tinfo;
  }

  # ----------------------------------------------------------------------
  # Start slaves - if needed
  # ----------------------------------------------------------------------
  if ( $tinfo->{'slave_num'} )
  {
    restore_slave_databases($tinfo->{'slave_num'});

    do_before_start_slave($tinfo);

    for ( my $idx= 0; $idx <  $tinfo->{'slave_num'}; $idx++ )
    {
      if ( ! $slave->[$idx]->{'pid'} )
      {
	mysqld_start($slave->[$idx],$tinfo->{'slave_opt'},
		     $tinfo->{'slave_mi'});

      }
    }

    # Save this test case information, so next can examine it
    $slave->[0]->{'running_slave_options'}= $tinfo;
  }

  # Wait for mysqld's to start
  foreach my $mysqld (@{$master},@{$slave})
  {

    next if !$mysqld->{'pid'};

    if (mysqld_wait_started($mysqld))
    {
      # failed to start
      $tinfo->{'comment'}=
	"Failed to start $mysqld->{'type'} mysqld $mysqld->{'idx'}";
      return 1;
    }
  }
  return 0;
}

#
# Run include/check-testcase.test
# Before a testcase, run in record mode, save result file to var
# After testcase, run and compare with the recorded file, they should be equal!
#
# RETURN VALUE
#  0 OK
#  1 Check failed
#
sub run_check_testcase ($$) {

  my $mode=     shift;
  my $mysqld=   shift;

  my $name= "check-" . $mysqld->{'type'} . $mysqld->{'idx'};

  my $args;
  mtr_init_args(\$args);

  mtr_add_arg($args, "--no-defaults");
  mtr_add_arg($args, "--silent");
  mtr_add_arg($args, "--tmpdir=%s", $opt_tmpdir);
  mtr_add_arg($args, "--character-sets-dir=%s", $path_charsetsdir);

  mtr_add_arg($args, "--socket=%s", $mysqld->{'path_sock'});
  mtr_add_arg($args, "--port=%d", $mysqld->{'port'});
  mtr_add_arg($args, "--database=test");
  mtr_add_arg($args, "--user=%s", $opt_user);
  mtr_add_arg($args, "--password=");

  mtr_add_arg($args, "-R");
  mtr_add_arg($args, "$opt_vardir/tmp/$name.result");

  if ( $mode eq "before" )
  {
    mtr_add_arg($args, "--record");
  }

  my $res = mtr_run_test($exe_mysqltest,$args,
	        "include/check-testcase.test", "", "", "");

  if ( $res == 1  and $mode eq "after")
  {
    mtr_run("diff",["-u",
		    "$opt_vardir/tmp/$name.result",
		    "$opt_vardir/tmp/$name.reject"],
	    "", "", "", "");
  }
  elsif ( $res )
  {
    mtr_error("Could not execute 'check-testcase' $mode testcase");
  }
  return $res;
}

##############################################################################
#
#  Report the features that were compiled in
#
##############################################################################

sub run_report_features () {
  my $args;

  if ( ! $glob_use_embedded_server )
  {
    mysqld_start($master->[0],[],[]);
    if ( ! $master->[0]->{'pid'} )
    {
      mtr_error("Can't start the mysqld server");
    }
    mysqld_wait_started($master->[0]);
  }

  my $tinfo = {};
  $tinfo->{'name'} = 'report features';
  $tinfo->{'result_file'} = undef;
  $tinfo->{'component_id'} = 'mysqld';
  $tinfo->{'path'} = 'include/report-features.test';
  $tinfo->{'timezone'}=  "GMT-3";
  $tinfo->{'slave_num'} = 0;
  $tinfo->{'master_opt'} = [];
  $tinfo->{'slave_opt'} = [];
  $tinfo->{'slave_mi'} = [];
  $tinfo->{'comment'} = 'report server features';
  run_mysqltest($tinfo);

  if ( ! $glob_use_embedded_server )
  {
    stop_all_servers();
  }
}


sub run_mysqltest ($) {
  my ($tinfo)= @_;
  my $exe= $exe_mysqltest;
  my $args;

  mtr_init_args(\$args);

  mtr_add_arg($args, "--no-defaults");
  mtr_add_arg($args, "--silent");
  mtr_add_arg($args, "--tmpdir=%s", $opt_tmpdir);
  mtr_add_arg($args, "--character-sets-dir=%s", $path_charsetsdir);
  mtr_add_arg($args, "--logdir=%s/log", $opt_vardir);

  # Log line number and time  for each line in .test file
  mtr_add_arg($args, "--mark-progress")
    if $opt_mark_progress;

  # component_id == mysqld
  {
    mtr_add_arg($args, "--socket=%s", $master->[0]->{'path_sock'});
    mtr_add_arg($args, "--port=%d", $master->[0]->{'port'});
    mtr_add_arg($args, "--database=test");
    mtr_add_arg($args, "--user=%s", $opt_user);
    mtr_add_arg($args, "--password=");
  }

  if ( $opt_ps_protocol )
  {
    mtr_add_arg($args, "--ps-protocol");
  }

  if ( $opt_sp_protocol )
  {
    mtr_add_arg($args, "--sp-protocol");
  }

  if ( $opt_view_protocol )
  {
    mtr_add_arg($args, "--view-protocol");
  }

  if ( $opt_cursor_protocol )
  {
    mtr_add_arg($args, "--cursor-protocol");
  }

  if ( $opt_strace_client )
  {
    $exe=  "strace";            # FIXME there are ktrace, ....
    mtr_add_arg($args, "-o");
    mtr_add_arg($args, "%s/log/mysqltest.strace", $opt_vardir);
    mtr_add_arg($args, "$exe_mysqltest");
  }

  if ( $opt_timer )
  {
    mtr_add_arg($args, "--timer-file=%s/log/timer", $opt_vardir);
  }

  if ( $opt_compress )
  {
    mtr_add_arg($args, "--compress");
  }

  if ( $opt_sleep )
  {
    mtr_add_arg($args, "--sleep=%d", $opt_sleep);
  }

  if ( $opt_debug )
  {
    mtr_add_arg($args, "--debug=d:t:A,%s/log/mysqltest.trace",
		$path_vardir_trace);
  }

  if ( $opt_ssl_supported )
  {
    mtr_add_arg($args, "--ssl-ca=%s/std_data/cacert.pem",
	        $glob_mysql_test_dir);
    mtr_add_arg($args, "--ssl-cert=%s/std_data/client-cert.pem",
	        $glob_mysql_test_dir);
    mtr_add_arg($args, "--ssl-key=%s/std_data/client-key.pem",
	        $glob_mysql_test_dir);
  }

  if ( $opt_ssl )
  {
    # Turn on SSL for _all_ test cases if option --ssl was used
    mtr_add_arg($args, "--ssl");
  }
  elsif ( $opt_ssl_supported )
  {
    mtr_add_arg($args, "--skip-ssl");
  }

  foreach my $arg ( @opt_extra_mysqltest_opt )
  {
    mtr_add_arg($args, "%s", $arg);
  }

  # ----------------------------------------------------------------------
  # If embedded server, we create server args to give mysqltest to pass on
  # ----------------------------------------------------------------------

  if ( $glob_use_embedded_server )
  {
    mysqld_arguments($args,$master->[0],$tinfo->{'master_opt'},[]);
  }

  # ----------------------------------------------------------------------
  # export MYSQL_TEST variable containing <path>/mysqltest <args>
  # ----------------------------------------------------------------------
  $ENV{'MYSQL_TEST'}=
    mtr_native_path($exe_mysqltest) . " " . join(" ", @$args);

  # ----------------------------------------------------------------------
  # Add arguments that should not go into the MYSQL_TEST env var
  # ----------------------------------------------------------------------

  if ( $opt_valgrind_mysqltest )
  {
    # Prefix the Valgrind options to the argument list.
    # We do this here, since we do not want to Valgrind the nested invocations
    # of mysqltest; that would mess up the stderr output causing test failure.
    my @args_saved = @$args;
    mtr_init_args(\$args);
    valgrind_arguments($args, \$exe);
    mtr_add_arg($args, "%s", $_) for @args_saved;
  }

  mtr_add_arg($args, "--test-file=%s", $tinfo->{'path'});

  # Number of lines of resut to include in failure report
  mtr_add_arg($args, "--tail-lines=20");

  if ( defined $tinfo->{'result_file'} ) {
    mtr_add_arg($args, "--result-file=%s", $tinfo->{'result_file'});
  }

  if ( $opt_record )
  {
    mtr_add_arg($args, "--record");
  }

  if ( $opt_client_gdb )
  {
    gdb_arguments(\$args, \$exe, "client");
  }
  elsif ( $opt_client_ddd )
  {
    ddd_arguments(\$args, \$exe, "client");
  }
  elsif ( $opt_client_debugger )
  {
    debugger_arguments(\$args, \$exe, "client");
  }

  if ( $opt_check_testcases )
  {
    foreach my $mysqld (@{$master}, @{$slave})
    {
      if ($mysqld->{'pid'})
      {
	run_check_testcase("before", $mysqld);
      }
    }
  }

  my $res = mtr_run_test($exe,$args,"","",$path_timefile,"");

  if ( $opt_check_testcases )
  {
    foreach my $mysqld (@{$master}, @{$slave})
    {
      if ($mysqld->{'pid'})
      {
	if (run_check_testcase("after", $mysqld))
	{
	  # Check failed, mark the test case with that info
	  $tinfo->{'check_testcase_failed'}= 1;
	}
      }
    }
  }

  return $res;

}


#
# Modify the exe and args so that program is run in gdb in xterm
#
sub gdb_arguments {
  my $args= shift;
  my $exe=  shift;
  my $type= shift;

  # Write $args to gdb init file
  my $str= join(" ", @$$args);
  my $gdb_init_file= "$opt_tmpdir/gdbinit.$type";

  # Remove the old gdbinit file
  unlink($gdb_init_file);

  if ( $type eq "client" )
  {
    # write init file for client
    mtr_tofile($gdb_init_file,
	       "set args $str\n" .
	       "break main\n");
  }
  else
  {
    # write init file for mysqld
    mtr_tofile($gdb_init_file, <<EOGDB );
set args $str
EOGDB
  }

  if ( $opt_manual_gdb )
  {
     print "\nTo start gdb for $type, type in another window:\n";
     print "gdb -cd $glob_mysql_test_dir -x $gdb_init_file $$exe\n";

     # Indicate the exe should not be started
     $$exe= undef;
     return;
  }

  $$args= [];
  mtr_add_arg($$args, "-title");
  mtr_add_arg($$args, "$type");
  mtr_add_arg($$args, "-e");

  if ( $exe_libtool )
  {
    mtr_add_arg($$args, $exe_libtool);
    mtr_add_arg($$args, "--mode=execute");
  }

  mtr_add_arg($$args, "gdb");
  mtr_add_arg($$args, "-x");
  mtr_add_arg($$args, "$gdb_init_file");
  mtr_add_arg($$args, "$$exe");

  $$exe= "xterm";
}


#
# Modify the exe and args so that program is run in ddd
#
sub ddd_arguments {
  my $args= shift;
  my $exe=  shift;
  my $type= shift;

  # Write $args to ddd init file
  my $str= join(" ", @$$args);
  my $gdb_init_file= "$opt_tmpdir/gdbinit.$type";

  # Remove the old gdbinit file
  unlink($gdb_init_file);

  if ( $type eq "client" )
  {
    # write init file for client
    mtr_tofile($gdb_init_file,
	       "set args $str\n" .
	       "break main\n");
  }
  else
  {
    # write init file for mysqld
    mtr_tofile($gdb_init_file,
	       "file $$exe\n" .
	       "set args $str\n");
  }

  if ( $opt_manual_ddd )
  {
     print "\nTo start ddd for $type, type in another window:\n";
     print "ddd -cd $glob_mysql_test_dir -x $gdb_init_file $$exe\n";

     # Indicate the exe should not be started
     $$exe= undef;
     return;
  }

  my $save_exe= $$exe;
  $$args= [];
  if ( $exe_libtool )
  {
    $$exe= $exe_libtool;
    mtr_add_arg($$args, "--mode=execute");
    mtr_add_arg($$args, "ddd");
  }
  else
  {
    $$exe= "ddd";
  }
  mtr_add_arg($$args, "--command=$gdb_init_file");
  mtr_add_arg($$args, "$save_exe");
}


#
# Modify the exe and args so that program is run in the selected debugger
#
sub debugger_arguments {
  my $args= shift;
  my $exe=  shift;
  my $debugger= $opt_debugger || $opt_client_debugger;

  if ( $debugger =~ /vcexpress|vc|devenv/ )
  {
    # vc[express] /debugexe exe arg1 .. argn

    # Add /debugexe and name of the exe before args
    unshift(@$$args, "/debugexe");
    unshift(@$$args, "$$exe");

    # Set exe to debuggername
    $$exe= $debugger;

  }
  elsif ( $debugger =~ /windbg/ )
  {
    # windbg exe arg1 .. argn

    # Add name of the exe before args
    unshift(@$$args, "$$exe");

    # Set exe to debuggername
    $$exe= $debugger;

  }
  elsif ( $debugger eq "dbx" )
  {
    # xterm -e dbx -r exe arg1 .. argn

    unshift(@$$args, $$exe);
    unshift(@$$args, "-r");
    unshift(@$$args, $debugger);
    unshift(@$$args, "-e");

    $$exe= "xterm";

  }
  else
  {
    mtr_error("Unknown argument \"$debugger\" passed to --debugger");
  }
}


#
# Modify the exe and args so that program is run in valgrind
#
sub valgrind_arguments {
  my $args= shift;
  my $exe=  shift;

  if ( $opt_callgrind)
  {
    mtr_add_arg($args, "--tool=callgrind");
    mtr_add_arg($args, "--base=$opt_vardir/log");
  }
  else
  {
    mtr_add_arg($args, "--tool=memcheck"); # From >= 2.1.2 needs this option
    mtr_add_arg($args, "--leak-check=yes");
    #mtr_add_arg($args, "--db-attach=yes");
    mtr_add_arg($args, "--num-callers=16");
    mtr_add_arg($args, "--suppressions=%s/valgrind.supp", $glob_mysql_test_dir)
      if -f "$glob_mysql_test_dir/valgrind.supp";
  }

  # Add valgrind options, can be overridden by user
  mtr_add_arg($args, '%s', $_) for (@valgrind_args);

  mtr_add_arg($args, $$exe);

  $$exe= $opt_valgrind_path || "valgrind";

  if ($exe_libtool)
  {
    # Add "libtool --mode-execute" before the test to execute
    # if running in valgrind(to avoid valgrinding bash)
    unshift(@$args, "--mode=execute", $$exe);
    $$exe= $exe_libtool;
  }
}


##############################################################################
#
#  Usage
#
##############################################################################

sub usage ($) {
  my $message= shift;

  if ( $message )
  {
    print STDERR "$message\n";
  }

  print <<HERE;

$0 [ OPTIONS ] [ TESTCASE ]

Options to control what engine/variation to run

  embedded-server       Use the embedded server, i.e. no mysqld daemons
  ps-protocol           Use the binary protocol between client and server
  cursor-protocol       Use the cursor protocol between client and server
                        (implies --ps-protocol)
  view-protocol         Create a view to execute all non updating queries
  sp-protocol           Create a stored procedure to execute all queries
  compress              Use the compressed protocol between client and server
  ssl                   Use ssl protocol between client and server
  skip-ssl              Dont start server with support for ssl connections
  bench                 Run the benchmark suite
  small-bench           Run the benchmarks with --small-tests --small-tables
  vs-config             Visual Studio configuration used to create executables
                        (default: MTR_VS_CONFIG environment variable)

Options to control directories to use
  benchdir=DIR          The directory where the benchmark suite is stored
                        (default: ../../mysql-bench)
  tmpdir=DIR            The directory where temporary files are stored
                        (default: ./var/tmp).
  vardir=DIR            The directory where files generated from the test run
                        is stored (default: ./var). Specifying a ramdisk or
                        tmpfs will speed up tests.
  mem                   Run testsuite in "memory" using tmpfs or ramdisk
                        Attempts to find a suitable location
                        using a builtin list of standard locations
                        for tmpfs (/dev/shm)
                        The option can also be set using environment
                        variable MTR_MEM=[DIR]

Options to control what test suites or cases to run

  force                 Continue to run the suite after failure
  do-test=PREFIX or REGEX
                        Run test cases which name are prefixed with PREFIX
                        or fulfills REGEX
  skip-test=PREFIX or REGEX
                        Skip test cases which name are prefixed with PREFIX
                        or fulfills REGEX
  start-from=PREFIX     Run test cases starting from test prefixed with PREFIX
  suite[s]=NAME1,..,NAMEN Collect tests in suites from the comma separated
                        list of suite names.
                        The default is: "$opt_suites_default"
  skip-rpl              Skip the replication test cases.
  big-test              Set the environment variable BIG_TEST, which can be
                        checked from test cases.
  combination="ARG1 .. ARG2" Specify a set of "mysqld" arguments for one
                        combination.
  skip-combination      Skip any combination options and combinations files

Options that specify ports

  master_port=PORT      Specify the port number used by the first master
  slave_port=PORT       Specify the port number used by the first slave
  mtr-build-thread=#    Specify unique collection of ports. Can also be set by
                        setting the environment variable MTR_BUILD_THREAD.

Options for test case authoring

  record TESTNAME       (Re)genereate the result file for TESTNAME
  check-testcases       Check testcases for sideeffects
  mark-progress         Log line number and elapsed time to <testname>.progress

Options that pass on options

  mysqld=ARGS           Specify additional arguments to "mysqld"

Options to run test on running server

  extern                Use running server for tests
  user=USER             User for connection to extern server
  socket=PATH           Socket for connection to extern server

Options for debugging the product

  client-ddd            Start mysqltest client in ddd
  client-debugger=NAME  Start mysqltest in the selected debugger
  client-gdb            Start mysqltest client in gdb
  ddd                   Start mysqld in ddd
  debug                 Dump trace output for all servers and client programs
  debugger=NAME         Start mysqld in the selected debugger
  gdb                   Start the mysqld(s) in gdb
  manual-debug          Let user manually start mysqld in debugger, before
                        running test(s)
  manual-gdb            Let user manually start mysqld in gdb, before running
                        test(s)
  manual-ddd            Let user manually start mysqld in ddd, before running
                        test(s)
  master-binary=PATH    Specify the master "mysqld" to use
  slave-binary=PATH     Specify the slave "mysqld" to use
  strace-client         Create strace output for mysqltest client
  max-save-core         Limit the number of core files saved (to avoid filling
                        up disks for heavily crashing server). Defaults to
                        $opt_max_save_core, set to 0 for no limit.

Options for coverage, profiling etc

  gcov                  FIXME
  gprof                 FIXME
  valgrind              Run the "mysqltest" and "mysqld" executables using
                        valgrind with default options
  valgrind-all          Synonym for --valgrind
  valgrind-mysqltest    Run the "mysqltest" and "mysql_client_test" executable
                        with valgrind
  valgrind-mysqld       Run the "mysqld" executable with valgrind
  valgrind-options=ARGS Deprecated, use --valgrind-option
  valgrind-option=ARGS  Option to give valgrind, replaces default option(s),
                        can be specified more then once
  valgrind-path=[EXE]   Path to the valgrind executable
  callgrind             Instruct valgrind to use callgrind

Misc options

  comment=STR           Write STR to the output
  notimer               Don't show test case execution time
  script-debug          Debug this script itself
  verbose               More verbose output
  start-and-exit        Only initialize and start the servers, using the
                        startup settings for the specified test case (if any)
  start-dirty           Only start the servers (without initialization) for
                        the specified test case (if any)
  fast                  Don't try to clean up from earlier runs
  reorder               Reorder tests to get fewer server restarts
  help                  Get this help text

  testcase-timeout=MINUTES Max test case run time (default $default_testcase_timeout)
  suite-timeout=MINUTES Max test suite run time (default $default_suite_timeout)
  warnings | log-warnings Pass --log-warnings to mysqld

  sleep=SECONDS         Passed to mysqltest, will be used as fixed sleep time
  client-bindir=PATH    Path to the directory where client binaries are located
  client-libdir=PATH    Path to the directory where client libraries are located

Deprecated options
  with-openssl          Deprecated option for ssl


HERE
  mtr_exit(1);

}

sub list_options ($) {
  my $hash= shift;

  for (keys %$hash) {
    s/(=.*|!)$//;
    s/\|/\n--/g;
    print "--$_\n";
  }

  mtr_exit(1);
}
