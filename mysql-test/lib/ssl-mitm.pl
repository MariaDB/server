#!/usr/bin/env perl

# mitm for mariadb procotol with ssl

use strict;
use warnings;
use autodie;
use Getopt::Long;
use Socket qw(PF_INET SOCK_STREAM INADDR_ANY INADDR_LOOPBACK sockaddr_in SO_REUSEADDR SOL_SOCKET);
use Net::SSLeay;

my $opt_listen_port;
my $opt_connect_port;
my $opt_key;
my $opt_cert;
my $opt_ca;

my %options=(
  'listen-on=i' => \$opt_listen_port,
  'connect-to=i' => \$opt_connect_port,
  'ssl-key=s' => \$opt_key,
  'ssl-cert=s' => \$opt_cert,
  'ssl-ca=s' => \$opt_ca,
);

GetOptions(%options) or usage("Can't read options");
die "not all options set" unless $opt_listen_port and $opt_connect_port
                             and $opt_key and $opt_cert and $opt_ca;

Net::SSLeay::load_error_strings();
Net::SSLeay::SSLeay_add_ssl_algorithms();

my $servctx = Net::SSLeay::CTX_new();
my $servssl = Net::SSLeay::new($servctx);

my $clictx = Net::SSLeay::CTX_new();
Net::SSLeay::CTX_load_verify_locations($clictx, $opt_ca, '');
Net::SSLeay::set_cert_and_key($clictx, $opt_cert, $opt_key);
my $clissl = Net::SSLeay::new($clictx);

socket(my $listen, PF_INET, SOCK_STREAM, getprotobyname('tcp'));
setsockopt($listen, SOL_SOCKET, SO_REUSEADDR, pack("l", 1));
bind($listen, sockaddr_in($opt_listen_port, INADDR_ANY));
listen($listen, 1);
warn "$$: listening";

print ">> MitM active <<\n";
close STDOUT;

fork and exit;
warn "$$: forked";

accept(my $client, $listen);
warn "$$: accepted";

socket(my $server, PF_INET, SOCK_STREAM, getprotobyname('tcp'));
connect($server, sockaddr_in($opt_connect_port, INADDR_LOOPBACK));
warn "$$: connected";

# handshake server -> client
recv $server, $_, 1e6, 0;
send $client, $_, 0;
warn "$$: handshake server -> client (".length.")";

# client replies with a dummy
recv $client, $_, 36, 0;
send $server, $_, 0;
warn "$$: client replies with a dummy (".length.")";

# SSL with server
Net::SSLeay::set_fd($servssl, fileno($server));
Net::SSLeay::connect($servssl);
warn "$$: ssl connect server";

# SSL accept client
Net::SSLeay::set_fd($clissl, fileno($client));
Net::SSLeay::accept($clissl);
warn "$$: ssl accept client";

while (1) {
  $_=Net::SSLeay::read($clissl) or last;
  warn "$$: ssl read client ".length.")";
  s/Detecting MitM/No MitM found!/;
  Net::SSLeay::write($servssl, $_) or last;
  warn "$$: ssl write server ".length.")";
  $_=Net::SSLeay::read($servssl) or last;
  warn "$$: ssl read server ".length.")";
  Net::SSLeay::write($clissl, $_) or last;
  warn "$$: ssl write client ".length.")";
}

close($server);
close($client);
close($listen);
warn "$$: ----------\n";
