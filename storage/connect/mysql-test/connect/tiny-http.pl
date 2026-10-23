#!/usr/bin/env perl

# Minimal HTTP server for tests: answers every request with the same file.
# The pid file is written once the port is listening.
#
# usage: tiny-http.pl port file pidfile

use strict;
use warnings;
use IO::Socket::INET;

die "usage: $0 port file pidfile\n" unless @ARGV == 3;
my ($port, $file, $pidfile)= @ARGV;

$SIG{PIPE}= 'IGNORE';

my $srv= IO::Socket::INET->new(LocalAddr => '127.0.0.1', LocalPort => $port,
                               ReuseAddr => 1, Listen => 5)
  or die "listen(127.0.0.1:$port): $!";

open(P, '>', $pidfile) or die "open(>$pidfile): $!";
print P "$$\n";
close P;

while (my $c= $srv->accept()) {
  binmode $c;
  if(<$c> =~ m@^GET [a-z0-9._/]+ HTTP/\d\.\d@i) {
    while (<$c>) { last if /^\r?$/ }
    open(F, '<', $file) or die "open(<$file): $!";
    binmode F;
    print $c "HTTP/1.0 200 OK\r\n\r\n", <F>;
    close F;
  }
  close $c;
}
die "accept(): $!";
