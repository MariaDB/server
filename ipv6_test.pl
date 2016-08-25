#!/usr/bin/perl
#    use Socket;
#    use Data::Dumper;
#    socket my $sock, PF_INET6, SOCK_STREAM, getprotobyname('tcp');
#    print Dumper($sock);
#    # eval{}, if there's no Socket::sockaddr_in6 at all, old Perl installation
#    connect $sock, sockaddr_in6(7, Socket::IN6ADDR_LOOPBACK);
#    print Dumper($@);
#    print Dumper($sock);
#    exit $@ eq "";

use Socket;

socket(my $sock, PF_INET6, SOCK_STREAM, 0)
     or die "socket: $!";

my $addr = sockaddr_in6(1715, Socket::IN6ADDR_LOOPBACK) or die "sockaddr_in6: $!";

bind $sock, $addr or die "bind: $!";
#my $err = $!;
#print $err;


#use IO::Socket;
#use IO::Socket::INET6;
#use Socket6;
#my $sock;
#$sock = IO::Socket::INET6->new(Domain =>  AF_INET6,
#                                 Proto     => udp,    
#                                 LocalAddr => 'localhost',
#                                 Broadcast => 1 ) 
#                             or die "Can't bind : $@\n";
#PeerPort  => 9999,
#                                 PeerAddr  => Socket6::inet_ntop(AF_INET6,in6addr_broadcast),
