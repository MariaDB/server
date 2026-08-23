#!/usr/bin/env perl

# mitm that counts roundtrips

use strict;
use warnings;
use autodie;
use Getopt::Long;
use Socket qw(PF_INET SOCK_STREAM INADDR_ANY INADDR_LOOPBACK sockaddr_in SO_REUSEADDR SOL_SOCKET);

my $opt_listen_port;
my $opt_connect_port;
my $opt_report;

my %options=(
  'listen-on=i' => \$opt_listen_port,
  'connect-to=i' => \$opt_connect_port,
  'report=s' =>  \$opt_report,
);

GetOptions(%options) or usage("Can't read options");
die "not all options set" unless $opt_listen_port and $opt_connect_port
                             and $opt_report;

socket(my $listen, PF_INET, SOCK_STREAM, getprotobyname('tcp'));
setsockopt($listen, SOL_SOCKET, SO_REUSEADDR, pack("l", 1));
bind($listen, sockaddr_in($opt_listen_port, INADDR_ANY));
listen($listen, 1);

open STDOUT, '>', $opt_report;
flock STDOUT, 2;

fork and exit;

accept(my $client, $listen);

socket(my $server, PF_INET, SOCK_STREAM, getprotobyname('tcp'));
connect($server, sockaddr_in($opt_connect_port, INADDR_LOOPBACK));

my $rin = '';
vec($rin, fileno($server),  1) = 1;
vec($rin, fileno($client),  1) = 1;

my $roundtrips=0;
my $did_s2c=1;
my $rout;

sub quit {
  close($server);
  close($client);
  close($listen);
  print "# Round-trips: ",$roundtrips>>1,"\n";
  exit;
};

sub s2c {
  recv $server, $_, 1e6, 0x40;
  quit() unless length;
  #print "S -> C: ",length,"\n";
  $roundtrips += not $did_s2c;
  $did_s2c= 1;
  send $client, $_, 0;
}

sub c2s {
  recv $client, $_, 1e6, 0x40;
  quit() unless length;
  #print "S <- C: ",length,"\n";
  $roundtrips += $did_s2c;
  $did_s2c= 0;
  send $server, $_, 0;
}

while(1) {
  select(undef, undef, undef, 0.010);
  my $nfound = select($rout=$rin, undef, undef, undef);
  if ($nfound == 1) {
    vec($rout, fileno($server), 1) ? s2c() : c2s();
  } else {
    die unless $nfound == 2;
    $did_s2c ? s2c() : c2s();
  }
}
