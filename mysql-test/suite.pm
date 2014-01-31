package My::Suite::Main;
use My::Platform;

@ISA = qw(My::Suite);

sub skip_combinations {
  my @combinations;

  # disable innodb/xtradb combinatons for configurations that were not built
  push @combinations, 'innodb_plugin' unless $ENV{HA_INNODB_SO};

  # if something is compiled in, it's innodb. xtradb is MODULE_ONLY:
  push @combinations, 'innodb' unless $::mysqld_variables{'innodb'} eq "ON";
  push @combinations, 'xtradb';

  # XtraDB is RECOMPILE_FOR_EMBEDDED, ha_xtradb.so cannot work with embedded server
  push @combinations, 'xtradb_plugin' if not $ENV{HA_XTRADB_SO}
                                          or $::opt_embedded_server;

  my %skip = ( 'include/have_innodb.combinations' => [ @combinations ],
               'include/have_xtradb.combinations' => [ @combinations ]);

  # don't run tests for the wrong platform
  $skip{'include/platform.combinations'} = [ (IS_WINDOWS) ? 'unix' : 'win' ];

  # as a special case, disable certain include files as a whole
  $skip{'include/not_embedded.inc'} = 'Not run for embedded server'
             if $::opt_embedded_server;

  $skip{'include/have_debug.inc'} = 'Requires debug build'
             unless defined $::mysqld_variables{'debug-dbug'};

  $skip{'include/have_ssl_communication.inc'} =
  $skip{'include/have_ssl_crypto_functs.inc'} = 'Requires SSL'
             unless defined $::mysqld_variables{'ssl-ca'};

  $skip{'include/have_example_plugin.inc'} = 'Need example plugin'
             unless $ENV{HA_EXAMPLE_SO};

  $skip{'include/not_windows.inc'} = 'Requires not Windows' if IS_WINDOWS;

  $skip{'t/plugin_loaderr.test'} = 'needs compiled-in innodb'
            unless $::mysqld_variables{'innodb'} eq "ON";

  # disable tests that use ipv6, if unsupported
  use Socket;
  $skip{'include/check_ipv6.inc'} = 'No IPv6'
             unless socket SOCK, PF_INET6, SOCK_STREAM, getprotobyname('tcp');
  close SOCK;

  %skip;
}

bless { };

