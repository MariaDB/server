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


#
# Rules to run first of all
#

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


sub get_basedir {
  my ($self, $group)= @_;
  my $basedir= $group->if_exist('basedir') ||
    $self->{ARGS}->{basedir};
  return $basedir;
}

sub get_testdir {
  my ($self, $group)= @_;
  my $testdir= $group->if_exist('testdir') ||
    $self->{ARGS}->{testdir};
  return $testdir;
}

# Retrive build directory (which is different from basedir in out-of-source build)
sub get_bindir {
  if (defined $ENV{MTR_BINDIR})
  {
    return $ENV{MTR_BINDIR};
  }
  my ($self, $group)= @_;
  return $self->get_basedir($group);
}

sub fix_charset_dir {
  my ($self, $config, $group_name, $group)= @_;
  return my_find_dir($self->get_basedir($group),
		     \@share_locations, "charsets");
}

sub fix_language {
  my ($self, $config, $group_name, $group)= @_;
  return my_find_dir($self->get_bindir($group),
		     \@share_locations);
}

sub fix_datadir {
  my ($self, $config, $group_name)= @_;
  my $vardir= $self->{ARGS}->{vardir};
  return "$vardir/$group_name/data";
}

sub fix_pidfile {
  my ($self, $config, $group_name, $group)= @_;
  my $vardir= $self->{ARGS}->{vardir};
  return "$vardir/run/$group_name.pid";
}

sub fix_port {
  my ($self, $config, $group_name, $group)= @_;
  return $self->{PORT}++;
}

sub fix_host {
  my ($self)= @_;
  'localhost'
}

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

sub fix_socket {
  my ($self, $config, $group_name, $group)= @_;
  # Put socket file in tmpdir
  my $dir= $self->{ARGS}->{tmpdir};
  return "$dir/$group_name.sock";
}

sub fix_tmpdir {
  my ($self, $config, $group_name, $group)= @_;
  my $dir= $self->{ARGS}->{tmpdir};
  return "$dir/$group_name";
}

sub fix_log_error {
  my ($self, $config, $group_name, $group)= @_;
  my $dir= $self->{ARGS}->{vardir};
  if ( $::opt_valgrind and $::opt_debug ) {
    return "$dir/log/$group_name.trace";
  } else {
    return "$dir/log/$group_name.err";
  }
}

sub fix_log {
  my ($self, $config, $group_name, $group)= @_;
  my $dir= dirname($group->value('datadir'));
  return "$dir/mysqld.log";
}

sub fix_bind_address {
  if (IS_WINDOWS) {
    return "*";
  } else {
    return "127.0.0.1";
  }
}
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


sub run_section_rules {
  my ($self, $config, $name, @rules)= @_;

  foreach my $group ( $config->like($name) ) {
    $self->run_rules_for_group($config, $group, @rules);
  }
}


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


1;

