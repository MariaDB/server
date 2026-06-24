#!/usr/bin/perl
# rpl_binlog_compress_overflow_row_proxy.pl -- MITM proxy for MDEV-39762 row event path.
# Patches un_len to 0xFFFFFFFC in the first WRITE_ROWS_COMPRESSED_EVENT.
# Usage: perl rpl_binlog_compress_overflow_row_proxy.pl <listen_port> <master_host> <master_port>

use strict;
use warnings;
use lib "$ENV{MYSQL_TEST_DIR}/lib";
use rpl_binlog_mitm;
use mtr_socket_relay;

my ($listen_port, $master_host, $master_port) = @ARGV;
die "Usage: $0 <listen_port> <master_host> <master_port>"
    unless $listen_port && $master_host && $master_port;


# NOTE: ROWS_HEADER_LEN_V1=8, COMMON_HEADER_LEN=19 from sql/log_event.h.
# If those change, the $tmp initialisation below must be updated.

my ($listen_sock, $slave, $master) = setup_relay($listen_port, $master_host, $master_port);
do_master_auth($master);
do_slave_handshake($slave);
relay_pre_dump($master, $slave);

# Phase 3: patch first WRITE_ROWS_COMPRESSED_EVENT.
# post_header_len per event type is read from FORMAT_DESCRIPTION_EVENT
# so we handle V1 (0xa6) and V2 (0xa9) correctly regardless of master version.
warn "PROXY_DEBUG: phase3 - event stream\n";
my %post_header_len;
my $patched = 0;

while (!$patched) {
    my ($eseq, $ehdr, $event_data) = read_packet($master);
    my $event_type = unpack('C', substr($event_data, 1 + EVENT_TYPE_OFFSET, 1));
    warn "PROXY_DEBUG: event type=0x" . sprintf('%02x', $event_type) . " len=" . length($event_data) . "\n";

    # Extract post_header_len for row event types from FDE
    if ($event_type == FORMAT_DESCRIPTION_EVENT) {
        # FDE body: 2(binlog_ver) + 50(server_ver) + 4(timestamp) + 1(hdr_len) = 57 bytes
        # before the post_header_len array (1 byte per type, 1-indexed).
        my $phl_start = 1 + COMMON_HEADER_LEN + 57;
        for my $type (WRITE_ROWS_COMPRESSED_EVENT, WRITE_ROWS_COMPRESSED_EVENT_V1) {
            if (length($event_data) > $phl_start + $type - 1) {
                $post_header_len{$type} = unpack('C',
                    substr($event_data, $phl_start + $type - 1, 1));
                warn "PROXY_DEBUG: FDE post_header_len[$type]=$post_header_len{$type}\n";
            }
        }
        send_packet($slave, $ehdr, $event_data);
        next;
    }

    if ($event_type == WRITE_ROWS_COMPRESSED_EVENT ||
        $event_type == WRITE_ROWS_COMPRESSED_EVENT_V1) {

        warn "PROXY_DEBUG: patching WRITE_ROWS_COMPRESSED_EVENT\n";

        # Mirror row_log_event_uncompress() walk to locate un_len.
        # tmp = src + common_header_len + ROWS_HEADER_LEN_V1 = 19 + 8 = 27
        # In $event_data the OK prefix byte is at [0], so add 1.
        my $tmp = 1 + COMMON_HEADER_LEN + ROWS_HEADER_LEN_V1;  # = 28

        my $ph_len = $post_header_len{$event_type} // ROWS_HEADER_LEN_V1;
        if ($ph_len == ROWS_HEADER_LEN_V2) {
            my $var_header_len = unpack('v', substr($event_data, $tmp, 2));
            warn "PROXY_DEBUG: V2 var_header_len=$var_header_len\n";
            $tmp += $var_header_len;
        }

        my $nfl_first = unpack('C', substr($event_data, $tmp, 1));
        my $nfl_size  = net_field_length_size($nfl_first);
        my $m_width   = net_field_length_value($event_data, $tmp);
        warn "PROXY_DEBUG: m_width=$m_width\n";
        $tmp += $nfl_size + int(($m_width + 7) / 8);
        
        my $flag_offset = $tmp;
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
