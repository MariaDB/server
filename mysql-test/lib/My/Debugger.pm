package My::Debugger;

use strict;
use warnings;
use Text::Wrap;
use Text::ParseWords;
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
# 2. terminal to use for interactive debuggers: configurable via the
#    --terminal option or the $MTR_TERM environment variable (default
#    "xterm -title {title} -e {command}").  {title} is the window title,
#    {command} the debugger invocation.
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
    exec => 'gdb --args',
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
    options => '_RR_TRACE_DIR={log} rr record {exe} {args} --loose-skip-innodb-use-native-aio --loose-innodb-flush-method=fsync',
    run => 'env',
    exec => 'rr record',
    pre => sub {
      push @::global_suppressions, qr/InnoDB: native AIO failed/;
      ::mtr_error('rr requires kernel.perf_event_paranoid <= 1')
        if ::mtr_grab_file('/proc/sys/kernel/perf_event_paranoid') > 1;
      $ENV{LSAN_OPTIONS}= "report_objects=1:" . ($ENV{LSAN_OPTIONS} || '');
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
my $opt_terminal;

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
  register_opt "exec-", $k, "Run every mysqltest --exec command"
    if $v->{exec};
}

# Terminal emulator used to run interactive ({term}) debuggers.  The template
# understands two placeholders: {title} (window title) and {command} (the
# debugger invocation).  --terminal takes precedence over $MTR_TERM; the
# default reproduces the former hard-coded xterm behaviour.
$opts{"terminal=s"} = \$opt_terminal;
$help .= wrap(sprintf("  %-23s", "terminal=TEMPL"), ' 'x25,
              "Terminal for interactive debuggers, with {title} and {command} ".
              "placeholders (default 'xterm -title {title} -e {command}', ".
              "also from \$MTR_TERM)\n");

sub term_template()
{
  my $t= $opt_terminal;
  undef $t if defined $t and $t eq ';';  # bare --terminal sentinel, ignore
  return $t || $ENV{MTR_TERM} || 'xterm -title {title} -e {command}';
}

# Expand the terminal template into an argv list that runs @cmd (the debugger
# and its arguments) in a window titled $title.  {command} expands to the
# words of @cmd; {title} is substituted textually within any word.
sub term_argv($@)
{
  my $title= shift;
  my @cmd= @_;
  my $templ= term_template();
  # Honour shell-style quoting: the argv is exec'd directly (no shell), so a
  # quoted argument in the template must become a single argv element.
  my @words= shellwords($templ);
  mtr_error "Malformed --terminal template: $templ" unless @words;
  my @argv;
  for my $word (@words)
  {
    if ($word eq '{command}')
    {
      push @argv, @cmd;
    }
    else
    {
      $word =~ s/\{title\}/$title/g;
      push @argv, $word;
    }
  }
  return @argv;
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
    my @argv= term_argv($type, $run, @$$args);
    $$exe = shift @argv;
    @$$args = @argv;
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

  # --exec-<debugger>: wrap every mysqltest '--exec' command line by exporting
  # MYSQLTEST_EXEC_WRAP, which do_exec() injects after any leading NAME=VALUE
  # assignments.  Independent of the mysqld/client/boot debuggers above.
  my %exec_options;
  for my $k (keys %debuggers) {
    my $val= $opt_vals{"exec-$k"};
    next unless $val;
    mtr_error "--exec-$k is not supported" unless $debuggers{$k}->{exec};
    $exec_options{$k}= $val;
  }
  if ((keys %exec_options) > 1) {
    mtr_error "Multiple exec debuggers specified: ",
        join (" ", map { "--exec-$_" } keys %exec_options);
  }
  if (my ($k)= keys %exec_options) {
    my $v= $debuggers{$k};
    # Run the debugger's one-time setup hook (a code ref, e.g. rr's
    # perf_event_paranoid check and suppression registration).  delete()
    # after calling it so it runs only once even when the same debugger is
    # also selected for another role (e.g. --rr together with --exec-rr) -
    # the hook has side effects (push @global_suppressions, env vars) that
    # must not be applied twice.
    if ($v->{pre}) {
      $v->{pre}->();
      delete $v->{pre};
    }
    my $wrap= $v->{exec};
    $ENV{_RR_TRACE_DIR}= "$::opt_vardir/log" if $k eq 'rr';
    if ($v->{term}) {
      # Here the wrapper is a shell string that do_exec() prepends before the
      # tool, so expand the template textually; {command} must be last for the
      # tool to end up as an argument of the debugger.
      (my $t= term_template()) =~ s/\{title\}/exec/g;
      $t =~ s/\{command\}/$wrap/;
      $wrap= $t;
    }
    $ENV{MYSQLTEST_EXEC_WRAP}= $wrap;
    $used= 1;
    $interactive ||= $v->{term};
  }

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
