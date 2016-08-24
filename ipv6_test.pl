#!/usr/bin/perl
    use Socket;
    use Data::Dumper;
    socket my $sock, PF_INET6, SOCK_STREAM, getprotobyname('tcp');
    print Dumper($sock);
    # eval{}, if there's no Socket::sockaddr_in6 at all, old Perl installation
    connect $sock, sockaddr_in6(7, Socket::IN6ADDR_LOOPBACK);
    print Dumper($@);
    print Dumper($sock);
    exit $@ eq "";
