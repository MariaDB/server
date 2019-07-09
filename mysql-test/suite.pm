package My::Suite::Main;
use My::Platform;

@ISA = qw(My::Suite);

sub skip_combinations {
  my @combinations;

  # disable innodb combinations for configurations that were not built
  push @combinations, 'innodb_plugin' unless $ENV{HA_INNODB_SO};

  push @combinations, 'innodb' unless $::mysqld_variables{'innodb'} eq "ON";

  my %skip = ( 'include/have_innodb.combinations' => [ @combinations ]);

  $skip{'include/innodb_encrypt_log.combinations'} = [ 'crypt' ]
                unless $ENV{DEBUG_KEY_MANAGEMENT_SO};

  # don't run tests for the wrong platform
  $skip{'include/platform.combinations'} = [ (IS_WINDOWS) ? 'unix' : 'win' ];

  $skip{'include/maybe_debug.combinations'} =
    [ defined $::mysqld_variables{'debug-dbug'} ? 'release' : 'debug' ];

  # and for the wrong word size
  # check for exact values, in case the default changes to be small everywhere
  my $longsysvar= $::mysqld_variables{'max-binlog-stmt-cache-size'};
  my %val_map= (
    '4294963200' => '64bit', # remember, it shows *what configuration to skip*
    '18446744073709547520' => '32bit'
  );
  die "unknown value max-binlog-stmt-cache-size=$longsysvar" unless $val_map{$longsysvar};
  $skip{'include/word_size.combinations'} = [ $val_map{$longsysvar} ];

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

  $skip{'main/plugin_loaderr.test'} = 'needs compiled-in innodb'
            unless $::mysqld_variables{'innodb'} eq "ON";

  $skip{'include/have_mariabackup.inc'} = 'Need mariabackup'
            unless ::have_mariabackup();

  $skip{'include/have_mariabackup.inc'} = 'Need ss'
            unless ::which("ss");

  $skip{'include/have_mariabackup.inc'} = 'Need socat or nc'
            unless $ENV{MTR_GALERA_TFMT};

  $skip{'include/have_garbd.inc'} = 'Need garbd'
            unless ::have_garbd();

  $skip{'include/have_file_key_management.inc'} = 'Needs file_key_management plugin'
            unless $ENV{FILE_KEY_MANAGEMENT_SO};

  # disable tests that use ipv6, if unsupported
  sub ipv6_ok() {
    use Socket;
    return 0 unless socket my $sock, PF_INET6, SOCK_STREAM, getprotobyname('tcp');
    $!="";
    # eval{}, if there's no Socket::sockaddr_in6 at all, old Perl installation <5.14
    eval { bind $sock, sockaddr_in6($::baseport, Socket::IN6ADDR_LOOPBACK) };
    return $@ eq "" && $! eq ""
  }
  $skip{'include/check_ipv6.inc'} = 'No IPv6' unless ipv6_ok();

  $skip{'main/openssl_6975.test'} = 'no or wrong openssl version'
    unless $::mysqld_variables{'version-ssl-library'} =~ /OpenSSL (\S+)/
       and $1 ge "1.0.1d" and $1 lt "1.1.1";

  $skip{'main/ssl_7937.combinations'} = [ 'x509v3' ]
    unless $::mysqld_variables{'version-ssl-library'} =~ /OpenSSL (\S+)/
       and $1 ge "1.0.2";

  $skip{'main/ssl_verify_ip.test'} = 'x509v3 support required'
    unless $::mysqld_variables{'version-ssl-library'} =~ /OpenSSL (\S+)/
       and $1 ge "1.0.2";

  %skip;
}

bless { };
