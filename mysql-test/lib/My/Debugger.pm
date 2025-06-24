package My::Debugger;

use strict;
use warnings;
use Text::Wrap;
use Cwd;
use My::Platform;
use mtr_report;

# 1. options to support:
#       --xxx[=ARGS]
#       --manual-xxx[=ARGS]
#       --client-xxx[=ARGS]
#       --boot-xxx[=ARGS]
#       TODO --manual-client-xxx[=ARGS]
#       TODO --manual-boot-xxx[=ARGS]
#       TODO --exec-xxx[=ARGS] (for $ENV{MYSQL}, etc)
#
#       ARGS is a semicolon-separated list of commands for the
#       command file. If the first command starts from '-' it'll
#       be for a command line, not for a command file.
#
# 2. terminal to use: xterm
#       TODO MTR_TERM="xterm -title {title} -e {command}"
#
# 3. debugger combinations are *not allowed*
#       (thus no --valgrind --gdb)
#
# 4. variables for the command line / file templates:
#       {vardir}   -> vardir
#       {exe} -> /path/to/binary/to/execute
#       {args} -> command-line arguments, "-quoted
#       {input}
#       {type} -> client, mysqld.1, etc
#       {script} -> vardir/tmp/{debugger}init.$type
#       {log} -> vardir/log/$type.{debugger}
#       {options} -> user options for the debugger.
#
#  if {options} isn't used, they're auto-placed before {exe}
#     or at the end if no {exe}

my %debuggers = (
  gdb => {
    term => 1,
    options => '-x {script} {exe}',
    script => 'set args {args} < {input}',
  },
  ddd => {
    interactive => 1,
    options => '--command {script} {exe}',
    script => 'set args {args} < {input}',
  },
  dbx => {
    term => 1,
    options => '-c "stop in main; run {exe} {args} < {input}"',
  },
  devenv => {
    interactive => 1,
    options => '/debugexe {exe} {args}',
  },
  windbg => {
    interactive => 1,
    options => '{exe} {args}',
  },
  lldb => {
    term => 1,
    options => '-s {script} {exe}',
    script => 'process launch --stop-at-entry -- {args}',
  },
  valgrind => {
    options => '--tool=memcheck --show-reachable=yes --leak-check=yes --num-callers=16 --quiet --suppressions='.cwd().'/valgrind.supp {exe} {args} --loose-wait-for-pos-timeout=1500',
    pre => sub {
      my $debug_libraries_path= "/usr/lib/debug";
      $ENV{LD_LIBRARY_PATH} .= ":$debug_libraries_path" if -d $debug_libraries_path;
    }
  },
  strace => {
    options => '-f -o {log} {exe} {args}',
  },
  rr => {
    options => '_RR_TRACE_DIR={log} rr record {exe} {args}',
    run => 'env',
    pre => sub {
      ::mtr_error('rr requires kernel.perf_event_paranoid <= 1')
        if ::mtr_grab_file('/proc/sys/kernel/perf_event_paranoid') > 1;
    }
  },
  valgdb => {
    term => 1,
    run => 'gdb',
    options => '-x {script} {exe}',
    script => <<EEE,
py
import subprocess,shlex,time
valg=subprocess.Popen(shlex.split("""valgrind --tool=memcheck --show-reachable=yes --leak-check=yes --num-callers=16 --quiet --suppressions=valgrind.supp --vgdb-error=0 {exe} {args} --loose-wait-for-pos-timeout=1500"""))
time.sleep(2)
gdb.execute("target remote | vgdb --pid=" + str(valg.pid))
EEE
    pre => sub {
      my $debug_libraries_path= "/usr/lib/debug";
      $ENV{LD_LIBRARY_PATH} .= ":$debug_libraries_path" if -d $debug_libraries_path;
    }
  },

  # aliases
  vsjitdebugger => 'windbg',
  ktrace => 'strace',
);

my %opts;
my %opt_vals;
my $debugger;
my $boot_debugger;
my $client_debugger;

my $help = "\n\nOptions for running debuggers\n\n";

for my $k (sort keys %debuggers) {
  my $v = $debuggers{$k};
  $v = $debuggers{$k} = $debuggers{$v} if not ref $v; # resolve aliases

  sub register_opt($$$) {
    my ($prefix, $name, $msg) = @_;
    $opts{"$prefix$name=s"} = \$opt_vals{$prefix.$name};
    $help .= wrap(sprintf("  %-23s", $prefix.$name), ' 'x25, "$msg under $name\n");
  }

  $v->{script} = '' unless $v->{script};
  $v->{options} =~ s/(\{exe\}|$)/ {options} $&/ unless $v->{options} =~ /\{options\}/;

  register_opt "", $k, "Start mysqld";
  register_opt "client-", $k, "Start mysqltest client";
  register_opt "boot-", $k, "Start bootstrap server";
  register_opt "manual-", "$k", "Before running test(s) let user manually start mariadbd";
}

sub subst($%) {
  use warnings FATAL => 'uninitialized';
  my ($templ, %vars) = @_;
  $templ =~ s/\{(\w+)\}/$vars{$1}/g;
  $templ;
}

sub do_args($$$$$) {
  my ($args, $exe, $input, $type, $opt) = @_;
  my $k = $opt =~ /^(?:client|boot|manual)-(.*)$/ ? $1 : $opt;
  my $v = $debuggers{$k};

  # on windows mtr args are quoted (for system), otherwise not (for exec)
  sub quote($) { $_[0] =~ /[; >]/ ? "\"$_[0]\"" : $_[0] }
  sub unquote($) { $_[0] =~ s/^"(.*)"$/$1/; $_[0] }
  sub quote_from_mtr($) { IS_WINDOWS() ? $_[0] : quote($_[0]) }
  sub unquote_for_mtr($) { IS_WINDOWS() ? $_[0] : unquote($_[0]) }

  my %vars = (
    vardir => $::opt_vardir,
    exe => $$exe,
    args => join(' ', map { quote_from_mtr $_ } @$$args,
                 '--loose-debug-gdb', '--loose-skip-stack-trace'),
    input => $input,
    script => "$::opt_vardir/tmp/${k}init.$type",
    log => "$::opt_vardir/log/$type.$k",
    options => '',
  );
  my @params = split /;/, $opt_vals{$opt};
  $vars{options} = shift @params  if @params and $params[0] =~ /^-/;

  my $script = join "\n", @params;
  if ($v->{script}) {
    ::mtr_tonewfile($vars{script}, subst($v->{script}, %vars)."\n".$script);
  } elsif ($script) {
    mtr_error "$k is not using a script file, nowhere to write the script \n---\n$script\n---";
  }

  my $options = subst($v->{options}, %vars);
  @$$args = map { unquote_for_mtr $_ } $options =~ /("[^"]+"|\S+)/g;
  my $run = $v->{run} || $k;

  if ($opt =~ /^manual-/) {
    print "\nTo start $k for $type, type in another window:\n";
    print "$run $options\n";
    $$exe= undef; # Indicate the exe should not be started
  } elsif ($v->{term}) {
    unshift @$$args, '-title', $type, '-e', $run;
    $$exe = 'xterm';
  } else {
    $$exe = $run;
  }
}

sub options() { %opts }
sub help() { $help }

sub fix_options(@) {
  my $re=join '|', keys %opts;
  $re =~ s/=s//g;
  # FIXME: what is '=;'? What about ':s' to denote optional argument in register_opt()
  map { $_ . (/^--($re)$/ and '=;') } @_;
}

sub pre_setup() {
  my $used;
  my $interactive;
  my %options;
  my %client_options;
  my %boot_options;

  my $embedded= $::opt_embedded_server ? ' with --embedded' : '';

  for my $k (keys %debuggers) {
    for my $opt ($k, "manual-$k", "boot-$k", "client-$k") {
      my $val= $opt_vals{$opt};
      if ($val) {
        $used = 1;
        $interactive ||= ($debuggers{$k}->{interactive} ||
                          $debuggers{$k}->{term} ||
                          ($opt =~ /^manual-/));
        if ($debuggers{$k}->{pre}) {
          $debuggers{$k}->{pre}->();
          delete $debuggers{$k}->{pre};
        }
        if ($opt eq $k) {
          $options{$opt}= $val;
          $client_options{$opt}= $val
            if $embedded;
        } elsif ($opt eq "manual-$k") {
          $options{$opt}= $val;
        } elsif ($opt eq "boot-$k") {
          $boot_options{$opt}= $val;
        } elsif ($opt eq "client-$k") {
          $client_options{$opt}= $val;
        }
      }
    }
  }

  if ((keys %options) > 1) {
    mtr_error "Multiple debuggers specified: ",
        join (" ", map { "--$_" } keys %options);
  }

  if ((keys %boot_options) > 1) {
    mtr_error "Multiple boot debuggers specified: ",
        join (" ", map { "--$_" } keys %boot_options);
  }

  if ((keys %client_options) > 1) {
    mtr_error "Multiple client debuggers specified: ",
        join (" ", map { "--$_" } keys %client_options);
  }

  $debugger= (keys %options)[0];
  $boot_debugger= (keys %boot_options)[0];
  $client_debugger= (keys %client_options)[0];

  if ($used) {
    $ENV{ASAN_OPTIONS}= 'abort_on_error=1:'.($ENV{ASAN_OPTIONS} || '');
    ::mtr_error("Can't use --extern when using debugger") if $ENV{USE_RUNNING_SERVER};

    $::opt_retry= 1;
    $::opt_retry_failure= 1;
    $::opt_testcase_timeout= ($interactive ? 24 : 4) * 60;      # in minutes
    $::opt_suite_timeout= 24 * 60;                              # in minutes
    $::opt_shutdown_timeout= ($interactive ? 24 * 60 : 3) * 60; # in seconds
    $::opt_start_timeout= $::opt_shutdown_timeout;              # in seconds
    $::opt_debug_sync_timeout= 3000;                            # in seconds
  }
}

sub setup_boot_args($$$) {
  my ($args, $exe, $input) = @_;
  do_args($args, $exe, $input, 'bootstrap', $boot_debugger)
    if defined $boot_debugger;
}

sub setup_client_args($$) {
  my ($args, $exe) = @_;
  do_args($args, $exe, IS_WINDOWS() ? 'NUL' : '/dev/null', 'client', $client_debugger)
    if defined $client_debugger;
}

sub setup_args($$$) {
  my ($args, $exe, $type) = @_;
  do_args($args, $exe, IS_WINDOWS() ? 'NUL' : '/dev/null', $type, $debugger)
    if defined $debugger;
}

1;
