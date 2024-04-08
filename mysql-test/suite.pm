package My::Suite::Main;
use My::Platform;

@ISA = qw(My::Suite);

sub skip_combinations {
  my %skip;

  $skip{'include/innodb_encrypt_log.combinations'} = [ 'crypt' ]
                unless $ENV{DEBUG_KEY_MANAGEMENT_SO};

  # don't run tests for the wrong platform
  if (IS_WINDOWS) {
    $skip{'include/platform.combinations'} = [ 'aix', 'unix' ];
  } elsif (IS_AIX) {
    $skip{'include/platform.combinations'} = [ 'win', 'unix' ];
  } else {
    $skip{'include/platform.combinations'} = [ 'aix', 'win' ];
  }

  $skip{'include/maybe_debug.combinations'} =
    [ defined $::mysqld_variables{'debug-dbug'} ? 'release' : 'debug' ];

  $skip{'include/have_debug.inc'} = 'Requires debug build'
             unless defined $::mysqld_variables{'debug-dbug'};

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

  $skip{'include/have_example_plugin.inc'} = 'Need example plugin'
             unless $ENV{HA_EXAMPLE_SO};

  $skip{'include/not_windows.inc'} = 'Requires not Windows' if IS_WINDOWS;
  $skip{'include/not_aix.inc'} = 'Requires not AIX' if IS_AIX;

  $skip{'main/plugin_loaderr.test'} = 'needs compiled-in innodb'
            unless $::mysqld_variables{'innodb'} eq "ON";

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

  # SSL is complicated
  my $ssl_lib= $::mysqld_variables{'version-ssl-library'};
  my $openssl_ver= $ssl_lib =~ /OpenSSL (\S+)/ ? $1 : "";

  $skip{'include/have_ssl_communication.inc'} =
  $skip{'include/have_ssl_crypto_functs.inc'} = 'Requires SSL' unless $ssl_lib;

  $skip{'main/openssl.test'} = 'openssl version greater than or equal to 1.0.1d required'
    unless $openssl_ver ge "1.0.1d";

  $skip{'main/ssl_7937.combinations'} = [ 'x509v3' ]
    unless $ssl_lib =~ /WolfSSL/ or $openssl_ver ge "1.0.2";

  $skip{'main/tlsv13.test'} = 'does not work with OpenSSL <= 1.1.1'
    unless $ssl_lib =~ /WolfSSL/ or $openssl_ver ge "3.0.0";

  $skip{'main/ssl_verify_ip.test'} = 'x509v3 support required'
    unless $openssl_ver ge "1.0.2";


  %skip;
}

bless { };
