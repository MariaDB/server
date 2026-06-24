package mtr_socket_relay;
use Exporter 'import';
our @EXPORT = qw(setup_relay);

use strict;
use warnings;
use IO::Socket::INET;
use POSIX qw(setsid);

# setup_relay($listen_port, $target_host, $target_port)
#
# Generic two-ended socket setup for a relay or proxy script.
# Binds a listen socket, forks and daemonizes so the caller returns
# to MTR immediately, accepts one inbound connection, opens one
# outbound connection to the target, and returns both sockets.
#
# SIGCHLD is set to IGNORE before forking to prevent zombie processes.
#
# Returns ($listen_sock, $inbound, $outbound).

sub setup_relay {
    my ($listen_port, $target_host, $target_port) = @_;

    my $listen_sock = IO::Socket::INET->new(
        LocalPort => $listen_port,
        Type      => SOCK_STREAM,
        ReuseAddr => 1,
        Listen    => 5,
        Timeout   => 60,
    ) or die "setup_relay: cannot bind $listen_port: $!";

    warn "PROXY_DEBUG: listening on $listen_port, target at $target_host:$target_port\n";

    local $SIG{CHLD} = 'IGNORE';
    close STDOUT;
    fork and exit;
    POSIX::setsid();
    open(STDOUT, '>', '/dev/null');

    my $inbound = $listen_sock->accept()
        or die "setup_relay: accept failed: $!";
    $inbound->autoflush(1);
    warn "PROXY_DEBUG: inbound connection accepted\n";

    my $outbound = IO::Socket::INET->new(
        PeerHost => $target_host,
        PeerPort => $target_port,
        Type     => SOCK_STREAM,
        Timeout  => 10,
    ) or die "setup_relay: cannot connect to $target_host:$target_port: $!";
    $outbound->autoflush(1);
    warn "PROXY_DEBUG: outbound connection established\n";

    return ($listen_sock, $inbound, $outbound);
}

1;
