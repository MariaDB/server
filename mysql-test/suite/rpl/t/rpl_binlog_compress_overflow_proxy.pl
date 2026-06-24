#!/usr/bin/perl
# rpl_binlog_compress_overflow_proxy.pl -- MITM proxy for MDEV-39762 query event path.
# Patches un_len to 0xFFFFFFFC in the first QUERY_COMPRESSED_EVENT (0xa5).
# Usage: perl rpl_binlog_compress_overflow_proxy.pl <listen_port> <master_host> <master_port>

use strict;
use warnings;
use lib "$ENV{MYSQL_TEST_DIR}/lib";
use rpl_binlog_mitm;
use mtr_socket_relay;

my ($listen_port, $master_host, $master_port) = @ARGV;
die "Usage: $0 <listen_port> <master_host> <master_port>"
    unless $listen_port && $master_host && $master_port;


# NOTE: offsets below reflect QUERY_HEADER_LEN=13, Q_DB_LEN_OFFSET=8,
# Q_STATUS_VARS_LEN_OFFSET=11 from sql/log_event.h. Update if post-header changes.
use constant QUERY_POST_HEADER_LEN    => 13;
use constant Q_DB_LEN_OFFSET          => 8;
use constant Q_STATUS_VARS_LEN_OFFSET => 11;
use constant QUERY_COMPRESSED_EVENT   => 165; # 0xa5

my ($listen_sock, $slave, $master) = setup_relay($listen_port, $master_host, $master_port);
do_master_auth($master);
do_slave_handshake($slave);
relay_pre_dump($master, $slave);

# Phase 3: patch first QUERY_COMPRESSED_EVENT
warn "PROXY_DEBUG: phase3 - event stream\n";
my $patched = 0;
while (!$patched) {
    my ($eseq, $ehdr, $event_data) = read_packet($master);
    my $event_type = unpack('C', substr($event_data, 1 + EVENT_TYPE_OFFSET, 1));
    warn "PROXY_DEBUG: event type=0x" . sprintf('%02x', $event_type) . " len=" . length($event_data) . "\n";

    if ($event_type == QUERY_COMPRESSED_EVENT) {
        warn "PROXY_DEBUG: patching QUERY_COMPRESSED_EVENT\n";

        # Locate un_len: 1(OK) + 19(hdr) + 13(post-hdr) + status_vars_len + db_len + 1(NUL)
        my $ph_start        = 1 + COMMON_HEADER_LEN;
        my $db_len          = unpack('C', substr($event_data, $ph_start + Q_DB_LEN_OFFSET, 1));
        my $status_vars_len = unpack('v', substr($event_data, $ph_start + Q_STATUS_VARS_LEN_OFFSET, 2));
        my $flag_offset     = 1 + COMMON_HEADER_LEN + QUERY_POST_HEADER_LEN
                            + $status_vars_len + $db_len + 1;
        patch_un_len(\$event_data, \$ehdr, $flag_offset);
        send_packet($slave, $ehdr, $event_data);
        $patched = 1;
    } else {
        send_packet($slave, $ehdr, $event_data);
    }
}

# Wait for slave to close connection after rejecting the patched event.
$slave->timeout(10);
my $buf;
read($slave, $buf, 1);
close($slave); close($master); close($listen_sock);
warn "PROXY_DEBUG: done\n";
exit 0;
