# -*- cperl -*-
# Copyright (c) 2007, 2011, Oracle and/or its affiliates
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; version 2
# of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA

package My::ConfigFactory;

use strict;
use warnings;
use Carp;

use My::Config;
use My::Find;
use My::Platform;

use File::Basename;

=head1 NAME

My::ConfigFactory - build a runtime server configuration for a test run

=head1 SYNOPSIS

  use My::ConfigFactory;

  my $config = My::ConfigFactory->new_config({
      basedir       => $basedir,
      vardir        => $opt_vardir,
      baseport      => $baseport,
      template_path => "include/default_my.cnf",
      # ... other args ...
  });

  # $config is a My::Config object, later written out as var/my.cnf

=head1 DESCRIPTION

C<My::ConfigFactory> turns a single my.cnf-format B<template> into the concrete
configuration used to launch the servers and clients of one test run. It reads
the template with L<My::Config>, then layers in generated values (ports,
directories, socket paths, server ids, ...) that depend on the current run.

The result is a L<My::Config> object that C<mariadb-test-run.pl> writes to
C<var/my.cnf>.

=head2 Which template is read

C<new_config> receives exactly one template in C<< $args->{template_path} >>.
The choice of I<which> template is made upstream in C<mtr_cases.pm> by
precedence (highest first); only the first that exists is used, and the
candidates are never merged with each other:

=over

=item 1. C<< <testname>.cnf >> in the test's C<t/> directory - per-test template

=item 2. the suite's C<my.cnf> - shared by all tests in that suite

=item 3. a built-in default when neither exists:

=over

=item * C<suite/rpl/my.cnf> for replication tests

=item * C<include/default_my.cnf> otherwise

=back

=back

The command line can also force a template with C<--defaults-file> (replacing
the selection above) or add one with C<--defaults-extra-file>.

Note: C<< $args->{extra_template_path} >> is passed in by C<mariadb-test-run.pl>
but is currently B<not> read by this module.

=head2 How the template is parsed and merged

L<My::Config> parses the template as my.cnf/INI format: C<[group]> sections
with C<option=value> lines. It resolves C<!include> and C<!includedir>
directives recursively, appending each included file's groups and options. The
shared building blocks under C<include/> are pulled in this way, e.g.:

    !include default_group_order.cnf   # fixes server startup order
    !include default_mysqld.cnf        # baseline [mysqld] options
    !include default_client.cnf        # baseline client options

(C<include/default_my.cnf> is itself just a chain of such includes.) A per-test
or per-suite C<.cnf> typically starts with one of these C<!include>s so it
I<inherits> the defaults and then overrides only what it needs.

Merging is last-value-wins: an option redefined later - by a subsequent
include, or in the template after an C<!include> - overrides the earlier value.

=head2 Generated settings (rules)

On top of the parsed template, C<new_config> runs rule sets that
insert-or-override options with values computed for this run:

=over

=item * C<@pre_rules> - general auto-options

=item * C<@mysqld_rules> - per C<[mysqld.N]>: port, datadir, socket, server-id, etc.

=item * group rules for C<[mysqlbinlog]>, C<[mysql_upgrade]>, C<[client]>, C<[mysqltest]>, ... - client-side connection settings

=item * C<@post_rules> - final adjustments

=back

Because rules use C<< $config->insert() >>, they override matching options that
came from the template.

=head2 Related files handled elsewhere

These affect a test's servers but are B<not> part of the template and are not
merged into the configuration:

=over

=item * C<.opt>, C<-master.opt>, C<-slave.opt> - flat command-line argument
lists (not my.cnf format); parsed by C<opts_from_file()> and applied as mysqld
startup arguments.

=item * C<.combinations> - my.cnf format and also parsed by L<My::Config>, but
in C<mtr_cases.pm> (not here); each C<[group]> becomes one test combination
whose options are added as mysqld startup arguments.

=back

=head1 USAGE

  # Constructing a configuration

  # Build a My::Config from a template plus this run's parameters.
  # Real example from mariadb-test-run.pl (default_mysqld).
  my $config= My::ConfigFactory->new_config({
    basedir       => $basedir,
    testdir       => $glob_mysql_test_dir,
    template_path => "include/default_my.cnf",
    vardir        => $opt_vardir,
    tmpdir        => $opt_tmpdir,
    baseport      => 0,
    user          => $opt_user,
    password      => '',
  });

  # Fetching a group

  # group($name) returns one My::Config::Group, or undef if absent
  my $mysqld= $config->group('mysqld.1')
    or mtr_error("Couldn't find mysqld.1 in default config");

  # Iterating groups

  # groups() lists every group
  for my $group ($config->groups()) {
    my $name = $group->name();      # e.g. "mysqld.1"
    my @opts = $group->options();   # this group's options
    if ($group == $mysqld) {        # same object group('mysqld.1') returned above
      print "This is mysqld.1 group!\n";
    }
  }

  # like($prefix) filters by name prefix - e.g. all server groups
  my @servers= $config->like('mysqld\.');

  # Generating values with fix_*

  # The fix_* generators are internal rule callbacks; most are called as
  # fix_x($self, $config, $group_name, $group) and need the factory's private
  # $self, so only these two - which ignore their arguments - are standalone:
  my $host= My::ConfigFactory::fix_host();          # always "localhost"
  my $bind= My::ConfigFactory::fix_bind_address();  # "127.0.0.1", or "*" on Windows

  # Reading an option

  # value($option) returns the value, or croaks if the option is missing
  my $log_error= $mysqld->value('log-error');

  # if_exist($option) returns undef instead of croaking when absent
  my $bind= $mysqld->if_exist('bind-address');

  # Writing the config to a file (var/my.cnf)

  # There is no built-in writer; mariadb-test-run.pl serializes the config
  # with mycnf_create(). option_groups() yields the "real" groups, skipping
  # the auto-generated ENV and OPT groups.
  open my $F, '>', "$opt_vardir/my.cnf" or die "$!";
  for my $group ($config->option_groups()) {
    print $F "[", $group->name(), "]\n";
    for my $option ($group->options()) {
      my $value= $option->value();
      print $F $option->name(), (defined $value ? "=$value" : ""), "\n";
    }
    print $F "\n";
  }
  close $F;

=head1 SUBROUTINES

The public entry point is C<new_config>. Everything else is internal and
documented here for maintainers: the rule engine, the pre-rules, the C<fix_*>
value generators and the C<post_*> checks.

=head2 Pre-rules (run first)

=over

=cut

#
# Rules to run first of all
#

=item add_opt_values(\%self, \%config)

Seed the auto-generated C<[OPT]> group (e.g. C<port>) and add
C<loose-skip-plugin-*> for optional plugins.

Called from: C<new_config>, via C<@pre_rules>.

=cut

sub add_opt_values {
  my ($self, $config)= @_;

  # add auto-options
  $config->insert('OPT', 'port'   => sub { fix_port($self, $config) });
  $config->insert('mysqld', "loose-skip-plugin-$_" => undef) for (@::optional_plugins);
}

my @pre_rules=
(
  \&add_opt_values,
);


my @share_locations= ("share/mariadb", "share/mysql", "sql/share", "share");


=back

=head2 Value generators (C<fix_*> and helpers)

Each C<fix_*> is a rule callback invoked as
C<< ($self, $config, $group_name, $group) >>, returning the value to insert for
one option.

=over

=item get_basedir(\%self, \%group)

Resolve the base directory, preferring an explicit value in the group, then
C<%ARGS>.

Called from: C<get_bindir> and C<fix_charset_dir>.

=cut

sub get_basedir {
  my ($self, $group)= @_;
  my $basedir= $group->if_exist('basedir') ||
    $self->{ARGS}->{basedir};
  return $basedir;
}

=item get_testdir(\%self, \%group)

Resolve the test directory, preferring an explicit value in the group, then
C<%ARGS>.

Called from: nowhere - currently unused.

=cut

sub get_testdir {
  my ($self, $group)= @_;
  my $testdir= $group->if_exist('testdir') ||
    $self->{ARGS}->{testdir};
  return $testdir;
}

=item get_bindir(\%self, \%group)

Resolve the build directory: C<$ENV{MTR_BINDIR}> if set, else the basedir.

Called from: C<fix_language>.

=cut

# Retrive build directory (which is different from basedir in out-of-source build)
sub get_bindir {
  if (defined $ENV{MTR_BINDIR})
  {
    return $ENV{MTR_BINDIR};
  }
  my ($self, $group)= @_;
  return $self->get_basedir($group);
}

=item fix_charset_dir(\%self, \%config, $group_name, \%group)

Locate the C<charsets> directory under the share locations.

Called from: C<run_rules_for_group>, as the C<character-sets-dir> rule in C<@mysqld_rules> and C<@client_rules>.

=cut

sub fix_charset_dir {
  my ($self, $config, $group_name, $group)= @_;
  return my_find_dir($self->get_basedir($group),
		     \@share_locations, "charsets");
}

=item fix_language(\%self, \%config, $group_name, \%group)

Locate the language/messages directory under the share locations.

Called from: C<run_rules_for_group>, as the C<lc-messages-dir> rule in C<@mysqld_rules>.

=cut

sub fix_language {
  my ($self, $config, $group_name, $group)= @_;
  return my_find_dir($self->get_bindir($group),
		     \@share_locations);
}

=item fix_datadir(\%self, \%config, $group_name)

Per-group data directory under C<vardir>.

Called from: C<run_rules_for_group>, as the C<datadir> rule in C<@mysqld_rules>.

=cut

sub fix_datadir {
  my ($self, $config, $group_name)= @_;
  my $vardir= $self->{ARGS}->{vardir};
  return "$vardir/$group_name/data";
}

=item fix_pidfile(\%self, \%config, $group_name, \%group)

Per-group pid file under C<vardir>.

Called from: C<run_rules_for_group>, as the C<pid-file> rule in C<@mysqld_rules>.

=cut

sub fix_pidfile {
  my ($self, $config, $group_name, $group)= @_;
  my $vardir= $self->{ARGS}->{vardir};
  return "$vardir/run/$group_name.pid";
}

=item fix_port(\%self, \%config, $group_name, \%group)

Hand out the next sequential port from C<< $self->{PORT} >>.

Called from: C<run_rules_for_group>, as the C<port> rule in C<@mysqld_rules>; also from C<add_opt_values> for the C<OPT> group.

=cut

sub fix_port {
  my ($self, $config, $group_name, $group)= @_;
  return $self->{PORT}++;
}

=item fix_host(\%self)

Return C<localhost>.

Called from: C<run_rules_for_group>, as the C<#host> rule in C<@mysqld_rules>.

=cut

sub fix_host {
  my ($self)= @_;
  'localhost'
}

=item is_unique(\%config, $name, $value)

Verify a value is not already used by another group.

Called from: C<fix_server_id>.

=cut

sub is_unique {
  my ($config, $name, $value)= @_;

  foreach my $group ( $config->groups() ) {
    if ($group->option($name)) {
      if ($group->value($name) eq $value){
	return 0;
      }
    }
  }
  return 1;
}

=item fix_server_id(\%self, \%config, $group_name, \%group)

Assign a unique C<server-id>; croak on a duplicate explicit id.

Called from: C<run_rules_for_group>, as the C<server-id> rule in C<@mysqld_rules>.

=cut

sub fix_server_id {
  my ($self, $config, $group_name, $group)= @_;
#define in the order that mysqlds are listed in my.cnf

  my $server_id= $group->if_exist('server-id');
  if (defined $server_id){
    if (!is_unique($config, 'server-id', $server_id)) {
      croak "The server-id($server_id) for '$group_name' is not unique";
    }
    return $server_id;
  }

  do {
    $server_id= $self->{SERVER_ID}++;
  } while(!is_unique($config, 'server-id', $server_id));

  #print "$group_name: server_id: $server_id\n";
  return $server_id;
}

=item fix_socket(\%self, \%config, $group_name, \%group)

Per-group unix socket under C<tmpdir>.

Called from: C<run_rules_for_group>, as the C<socket> rule in C<@mysqld_rules>.

=cut

sub fix_socket {
  my ($self, $config, $group_name, $group)= @_;
  # Put socket file in tmpdir
  my $dir= $self->{ARGS}->{tmpdir};
  return "$dir/$group_name.sock";
}

=item fix_tmpdir(\%self, \%config, $group_name, \%group)

Per-group tmp directory under C<tmpdir>.

Called from: C<run_rules_for_group>, as the C<tmpdir> rule in C<@mysqld_rules>.

=cut

sub fix_tmpdir {
  my ($self, $config, $group_name, $group)= @_;
  my $dir= $self->{ARGS}->{tmpdir};
  return "$dir/$group_name";
}

=item fix_log_error(\%self, \%config, $group_name, \%group)

Error log path (or C<.trace> under valgrind+debug).

Called from: C<run_rules_for_group>, as the C<log-error> rule in C<@mysqld_rules>.

=cut

sub fix_log_error {
  my ($self, $config, $group_name, $group)= @_;
  my $dir= $self->{ARGS}->{vardir};
  if ( $::opt_valgrind and $::opt_debug ) {
    return "$dir/log/$group_name.trace";
  } else {
    return "$dir/log/$group_name.err";
  }
}

=item fix_log(\%self, \%config, $group_name, \%group)

General query log path.

Called from: C<run_rules_for_group>, as the C<general-log-file> rule in C<@mysqld_rules>.

=cut

sub fix_log {
  my ($self, $config, $group_name, $group)= @_;
  my $dir= dirname($group->value('datadir'));
  return "$dir/mysqld.log";
}

=item fix_bind_address()

Return the bind address (C<*> on Windows, C<127.0.0.1> elsewhere).

Called from: C<run_rules_for_group>, as the C<bind-address> rule in C<@mysqld_rules>.

=cut

sub fix_bind_address {
  if (IS_WINDOWS) {
    return "*";
  } else {
    return "127.0.0.1";
  }
}

=item fix_log_slow_queries(\%self, \%config, $group_name, \%group)

Slow query log path.

Called from: C<run_rules_for_group>, as the C<slow-query-log-file> rule in C<@mysqld_rules>.

=cut

sub fix_log_slow_queries {
  my ($self, $config, $group_name, $group)= @_;
  my $dir= dirname($group->value('datadir'));
  return "$dir/mysqld-slow.log";
}

#
# Rules to run for each mysqld in the config
#  - will be run in order listed here
#
my @mysqld_rules=
  (
 { 'basedir' => sub { return shift->{ARGS}->{basedir}; } },
 { 'tmpdir' => \&fix_tmpdir },
 { 'character-sets-dir' => \&fix_charset_dir },
 { 'lc-messages-dir' => \&fix_language },
 { 'datadir' => \&fix_datadir },
 { 'pid-file' => \&fix_pidfile },
 { '#host' => \&fix_host },
 { 'port' => \&fix_port },
 { 'socket' => \&fix_socket },
 { 'log-error' => \&fix_log_error },
 { 'general-log' => 1 },
 { 'plugin-dir' => sub { $::plugindir } },
 { 'general-log-file' => \&fix_log },
 { 'slow-query-log' => 1 },
 { 'slow-query-log-file' => \&fix_log_slow_queries },
 { '#user' => sub { return shift->{ARGS}->{user} || ""; } },
 { '#password' => sub { return shift->{ARGS}->{password} || ""; } },
 { 'server-id' => \&fix_server_id, },
 { 'bind-address' => \&fix_bind_address },
  );

#
# Rules to run for [client] section
#  - will be run in order listed here
#
my @client_rules=
(
 { 'character-sets-dir' => \&fix_charset_dir },
 { 'plugin-dir' => sub { $::client_plugindir } },
);


#
# Rules to run for [mysqltest] section
#  - will be run in order listed here
#
my @mysqltest_rules=
(
);


#
# Rules to run for [mysqlbinlog] section
#  - will be run in order listed here
#
my @mysqlbinlog_rules=
(
);


#
# Rules to run for [mysql_upgrade] section
#  - will be run in order listed here
#
my @mysql_upgrade_rules=
(
 { 'tmpdir' => sub { return shift->{ARGS}->{tmpdir}; } },
);


=back

=head2 Post-rules (run last)

=over

=item post_check_client_group(\%self, \%config, $client_group_name, $mysqld_group_name)

Generate one C<[client...]> group, copying port / socket / host / user /
password from its mysqld.

Called from: C<post_check_client_groups>.

=cut

#
# Generate a [client.<suffix>] group to be
# used for connecting to [mysqld.<suffix>]
#
sub post_check_client_group {
  my ($self, $config, $client_group_name, $mysqld_group_name)= @_;


  #  Settings needed for client, copied from its "mysqld"
  my %client_needs=
    (
     port       => 'port',
     socket     => 'socket',
     host       => '#host',
     user       => '#user',
     password   => '#password',
    );
  my $group_to_copy_from= $config->group($mysqld_group_name);
  while (my ($name_to, $name_from)= each( %client_needs )) {
    my $option= $group_to_copy_from->option($name_from);

    if (! defined $option){
      #print $config;
      croak "Could not get value for '$name_from' for test $self->{testname}";
    }
    $config->insert($client_group_name, $name_to, $option->value())
  }
}


=item post_check_client_groups(\%self, \%config)

Generate a C<[client]> group pointing at the first C<[mysqld.N]>, plus a
matching C<[client.N]> per mysqld.

Called from: C<new_config>, via C<@post_rules>.

=cut

sub post_check_client_groups {
 my ($self, $config)= @_;

 my $first_mysqld= $config->first_like('mysqld\.');

 return unless $first_mysqld;

 # Always generate [client] pointing to the first
 # [mysqld.<suffix>]
 $self->post_check_client_group($config,
				'client',
				$first_mysqld->name());

 # Then generate [client.<suffix>] for each [mysqld.<suffix>]
 foreach my $mysqld ( $config->like('mysqld\.') ) {
   $self->post_check_client_group($config,
				  'client'.$mysqld->after('mysqld'),
				  $mysqld->name())
 }

}


=item post_check_embedded_group(\%self, \%config)

When running embedded, build an C<[embedded]> group from the default
C<[mysqld]> and the first C<[mysqld.N]>, skipping options that don't apply to
the embedded server.

Called from: C<new_config>, via C<@post_rules>.

=cut

#
# Generate [embedded] by copying the values
# needed from the default [mysqld] section
# and from first [mysqld.<suffix>]
#
sub post_check_embedded_group {
  my ($self, $config)= @_;

  return unless $self->{ARGS}->{embedded};

  my $mysqld= $config->group('mysqld') or
    croak "Can't run with embedded, config has no default mysqld section";

  my $first_mysqld= $config->first_like('mysqld\.') or
    croak "Can't run with embedded, config has no mysqld";

  my %no_copy = map { $_ => 1 }
    (
     'log-error', # Embedded server writes stderr to mysqltest's log file
     'slave-net-timeout', # Embedded server are not build with replication
    );

  foreach my $option ( $mysqld->options(), $first_mysqld->options() ) {
    # Don't copy options whose name is in "no_copy" list
    next if $no_copy{$option->name()};

    $config->insert('embedded', $option->name(), $option->value())
  }

}


=item resolve_at_variable(\%self, \%config, \%group, \%option)

Expand C<@group.option> references in one option's value, substituting the
referenced option's value.

Called from: C<post_fix_resolve_at_variables>.

=cut

sub resolve_at_variable {
  my ($self, $config, $group, $option)= @_;
  local $_ = $option->value();
  my ($res, $after);

  while (m/(.*?)\@((?:\w+\.)+)(#?[-\w]+)/g) {
    my ($before, $group_name, $option_name)= ($1, $2, $3);
    $after = $';
    chop($group_name);

  my $from_group= $config->group($group_name)
    or croak "There is no group named '$group_name' that ",
      "can be used to resolve '$option_name' for test '$self->{testname}'";

    my $value= $from_group->value($option_name);
    if (!defined($value))
    {
      ::mtr_verbose("group: $group_name  option_name: $option_name is undefined");
    }
    else
    {
      $res .= $before.$value;
    }
  }
  $res .= $after;

  $option->{value}= $res;
}


=item post_fix_resolve_at_variables(\%self, \%config)

Expand C<@group.option> references across all option values.

Called from: C<new_config>, via C<@post_rules>.

=cut

sub post_fix_resolve_at_variables {
  my ($self, $config)= @_;

  foreach my $group ( $config->groups() ) {
    foreach my $option ( $group->options()) {
      next unless defined $option->value();

      $self->resolve_at_variable($config, $group, $option)
	if ($option->value() =~ /\@/);
    }
  }
}

#
# Rules to run last of all
#
my @post_rules=
(
 \&post_check_client_groups,
 \&post_fix_resolve_at_variables,
 \&post_check_embedded_group,
);


=back

=head2 Rule engine

=over

=item run_rules_for_group(\%self, \%config, \%group, @rules)

Apply @rules to a single group. Each rule is a hashref
C<< { option => value_or_coderef } >> and fires only if the option is not
already set; a coderef is called as
C<< $rule->($self, $config, $group_name, $group) >> and its defined return value
is inserted. This is what lets template values win over generated defaults.

Called from: C<new_config> and C<run_section_rules>.

=cut

sub run_rules_for_group {
  my ($self, $config, $group, @rules)= @_;
  foreach my $hash ( @rules ) {
    while (my ($option, $rule)= each( %{$hash} )) {
      # Only run this rule if the value is not already defined
      if (!$config->exists($group->name(), $option)) {
	my $value;
	if (ref $rule eq "CODE") {
	  # Call the rule function
	  $value= &$rule($self, $config, $group->name(),
			 $config->group($group->name()));
	} else {
	  $value= $rule;
	}
	if (defined $value) {
	  $config->insert($group->name(), $option, $value, 1);
	}
      }
    }
  }
}


=item run_section_rules(\%self, \%config, $name, @rules)

Apply @rules to every group whose name matches C</^$name/> (e.g. all
C<mysqld.*> sections).

Called from: C<new_config>.

=cut

sub run_section_rules {
  my ($self, $config, $name, @rules)= @_;

  foreach my $group ( $config->like($name) ) {
    $self->run_rules_for_group($config, $group, @rules);
  }
}


=back

=head2 Public

=over

=item new_config($class, \%args)

Class method described under L</DESCRIPTION>. Required args: C<basedir>,
C<baseport>, C<vardir>, C<template_path>. Returns a resolved L<My::Config>.

Called from: C<mariadb-test-run.pl> (e.g. C<default_mysqld>) and C<testMyConfigFactory.t>.

=cut

sub new_config {
  my ($class, $args)= @_;

  my @required_args= ('basedir', 'baseport', 'vardir', 'template_path');

  foreach my $required ( @required_args ) {
    croak "you must pass '$required'" unless defined $args->{$required};
  }

  # Open the config template
  my $config= My::Config->new($args->{'template_path'});
  my $self= bless {
		   CONFIG       => $config,
		   ARGS         => $args,
		   PORT         => $args->{baseport},
		   SERVER_ID    => 1,
                   testname     => $args->{testname},
		  }, $class;

  # Run pre rules
  foreach my $rule ( @pre_rules ) {
    &$rule($self, $config);
  }

  $self->run_section_rules($config,
			   'mysqld\.',
			   @mysqld_rules);

  # [mysqlbinlog] need additional settings
  $self->run_rules_for_group($config,
			     $config->insert('mysqlbinlog'),
			     @mysqlbinlog_rules);

  # [mysql_upgrade] need additional settings
  $self->run_rules_for_group($config,
			     $config->insert('mysql_upgrade'),
			     @mysql_upgrade_rules);

  # Additional rules required for [client]
  $self->run_rules_for_group($config,
			     $config->insert('client'),
			     @client_rules);


  # Additional rules required for [mysqltest]
  $self->run_rules_for_group($config,
			     $config->insert('mysqltest'),
			     @mysqltest_rules);

  {
    # Run post rules
    foreach my $rule ( @post_rules ) {
      &$rule($self, $config);
    }
  }

  return $config;
}


=back

=head1 SEE ALSO

L<My::Config> - the my.cnf-format parser used here.

=cut

1;
