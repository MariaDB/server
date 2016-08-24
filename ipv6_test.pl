#!/usr/bin/perl
    use Socket;
    return 0 unless socket my $sock, PF_INET6, SOCK_STREAM, getprotobyname('tcp');
    # eval{}, if there's no Socket::sockaddr_in6 at all, old Perl installation
    eval { connect $sock, sockaddr_in6(7, Socket::IN6ADDR_LOOPBACK) };
    exit $@ eq "";
