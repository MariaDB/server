#!/usr/bin/perl
# rpl_binlog_mitm.pl -- shared MySQL protocol helpers for replication MITM tests.
#
# Provides packet I/O, net_field_length decoding, and the three phases common
# to all replication MITM proxies: master auth, slave handshake, pre-dump relay.
# Each proxy script use() this file and implements its own event patching.
package rpl_binlog_mitm;
use Exporter 'import';
our @EXPORT = qw(
    EVENT_TYPE_OFFSET EVENT_LEN_OFFSET COMMON_HEADER_LEN
    ROWS_HEADER_LEN_V1 ROWS_HEADER_LEN_V2
    FORMAT_DESCRIPTION_EVENT
    WRITE_ROWS_COMPRESSED_EVENT WRITE_ROWS_COMPRESSED_EVENT_V1
    BAD_LEN_UINT32
    read_packet send_packet
    net_field_length_size net_field_length_value
    do_master_auth do_slave_handshake relay_pre_dump patch_un_len

);
use strict;
use warnings;

# ---------------------------------------------------------------------------
# Constants (from sql/log_event.h)
# ---------------------------------------------------------------------------
use constant EVENT_TYPE_OFFSET  => 4;
use constant EVENT_LEN_OFFSET   => 9;
use constant COMMON_HEADER_LEN  => 19;
use constant ROWS_HEADER_LEN_V1 => 8;
use constant ROWS_HEADER_LEN_V2 => 10;

use constant FORMAT_DESCRIPTION_EVENT       => 15;  # 0x0f
use constant WRITE_ROWS_COMPRESSED_EVENT    => 169; # 0xa9
use constant WRITE_ROWS_COMPRESSED_EVENT_V1 => 166; # 0xa6

# BAD_LEN_UINT32: a uint32 value exceeding MAX_MAX_ALLOWED_PACKET (1GB).
# Used to trigger integer overflow in length field validation tests.
# Value chosen so ALIGN_SIZE() truncation wraps to a small non-zero value.

use constant BAD_LEN_UINT32 => 0xFFFFFFFC;

# ---------------------------------------------------------------------------
# Packet I/O
# ---------------------------------------------------------------------------
sub read_packet {
    my ($sock) = @_;
    my $hdr = '';
    my $n = read($sock, $hdr, 4);
    die "read_packet: short header (got " . ($n // 'undef') . ")"
        unless defined($n) && $n == 4;
    my ($l0, $l1, $l2, $seq) = unpack('CCCC', $hdr);
    my $len = $l0 | ($l1 << 8) | ($l2 << 16);
    my $data = '';
    if ($len > 0) {
        $n = read($sock, $data, $len);
        die "read_packet: short body" unless defined($n) && $n == $len;
    }
    return ($seq, $hdr, $data);
}

sub send_packet {
    my ($sock, $hdr, $data) = @_;
    print $sock $hdr . $data;
}

# ---------------------------------------------------------------------------
# net_field_length -- mirrors net_field_length() in sql/pack.cc
# NOTE: if the encoding changes, update both helpers below.
# ---------------------------------------------------------------------------
sub net_field_length_size {
    my ($first) = @_;
    return 1 if $first < 251;
    return 3 if $first == 252;
    return 4 if $first == 253;
    return 9;
}

sub net_field_length_value {
    my ($data, $offset) = @_;
    my $first = unpack('C', substr($data, $offset, 1));
    return $first                                                if $first < 251;
    return unpack('v',  substr($data, $offset + 1, 2))          if $first == 252;
    return unpack('V',  substr($data, $offset + 1, 3) . "\x00") if $first == 253;
    return 0;
}

# ---------------------------------------------------------------------------
# Phase 1a -- authenticate to the real master as root (no password)
# ---------------------------------------------------------------------------
sub do_master_auth {
    my ($master) = @_;
    my ($seq, $hdr, $greeting) = read_packet($master);
    warn "PROXY_DEBUG: master greeting seq=$seq len=" . length($greeting) . "\n";

    my $caps = 0x0001 | 0x0200 | 0x8000;
    my $auth = pack('V', $caps) . pack('V', 16777216) . pack('C', 8)
             . "\x00" x 23 . "root\x00" . "\x00";

    print $master pack('CCC', length($auth) & 0xFF,
                              (length($auth) >> 8) & 0xFF,
                              (length($auth) >> 16) & 0xFF)
                . pack('C', 1) . $auth;

    my ($aseq, $ahdr, $ok) = read_packet($master);
    my $first = unpack('C', substr($ok, 0, 1));
    warn "PROXY_DEBUG: master auth response=0x" . sprintf('%02x', $first) . "\n";
    die "PROXY_DEBUG: master auth failed" if $first == 0xff;

}

# ---------------------------------------------------------------------------
# Phase 1b -- send a fresh greeting to the slave and accept any credentials
# ---------------------------------------------------------------------------
sub do_slave_handshake {
    my ($slave) = @_;

    my $greeting =
          "\x0a" . "10.11.0-MariaDB\x00"
        . pack('V', 100)
        . "12345678\x00"
        . pack('v', 0x0001 | 0x0002 | 0x0004 | 0x0200 | 0x8000)
        . "\x08" . pack('v', 2) . pack('v', 0) . pack('C', 21)
        . "\x00" x 10
        . "123456789012\x00";

    my $len = length($greeting);
    print $slave pack('CCC', $len & 0xFF, ($len >> 8) & 0xFF, ($len >> 16) & 0xFF)
               . pack('C', 0) . $greeting;

    my ($slave_seq, $hdr, $auth) = read_packet($slave);
    warn "PROXY_DEBUG: slave auth seq=$slave_seq\n";

    my $ok = "\x00\x00\x00\x02\x00\x00";
    print $slave pack('CCC', length($ok), 0, 0) . pack('C', $slave_seq + 1) . $ok;
    warn "PROXY_DEBUG: sent OK to slave\n";

}

# ---------------------------------------------------------------------------
# Phase 2 -- relay all pre-dump packets transparently until COM_BINLOG_DUMP.
# Returns when COM_BINLOG_DUMP (0x12) has been forwarded to master.
# ---------------------------------------------------------------------------
sub relay_pre_dump {
    my ($master, $slave) = @_;
    warn "PROXY_DEBUG: phase2 - pre-dump relay\n";

    while (1) {
        my ($seq, $hdr, $cmd_data) = read_packet($slave);
        my $cmd = unpack('C', substr($cmd_data, 0, 1));
        warn "PROXY_DEBUG: slave cmd=0x" . sprintf('%02x', $cmd) . "\n";

        send_packet($master, $hdr, $cmd_data);
        last if $cmd == 0x12;  # COM_BINLOG_DUMP

        # relay master's response (OK/ERR or full result set)
        my $field_count;
        my $in_rows = 0;
        while (1) {
            my ($rseq, $rhdr, $resp) = read_packet($master);
            send_packet($slave, $rhdr, $resp);
            my $first = unpack('C', substr($resp, 0, 1));
            if (!defined $field_count) {
                last if $first == 0x00 || $first == 0xff;
                $field_count = $first;
            } elsif (!$in_rows) {
                $in_rows = 1 if $first == 0xfe && length($resp) < 9;
            } else {
                last if $first == 0xfe && length($resp) < 9;
            }
        }
    }
    warn "PROXY_DEBUG: COM_BINLOG_DUMP forwarded, entering event stream\n";
}

# ---------------------------------------------------------------------------
# patch_un_len($event_data_ref, $ehdr_ref, $flag_offset)
#
# Patches the un_len field at $flag_offset in a compressed binlog event.
# Updates the event length in the common header and the MySQL packet header.
# Operates on references so changes are reflected in the caller.
# ---------------------------------------------------------------------------
sub patch_un_len {
    my ($event_data_ref, $ehdr_ref, $flag_offset) = @_;

    my $flag_byte     = unpack('C', substr($$event_data_ref, $flag_offset, 1));
    my $old_len_bytes = $flag_byte & 0x07;

    substr($$event_data_ref, $flag_offset, 1 + $old_len_bytes) =
        "\x84" . pack('V', BAD_LEN_UINT32);

    my $delta = 4 - $old_len_bytes;
    if ($delta) {
        my $old_len = unpack('V', substr($$event_data_ref, 1 + EVENT_LEN_OFFSET, 4));
        substr($$event_data_ref, 1 + EVENT_LEN_OFFSET, 4) =
            pack('V', $old_len + $delta);
    }

    my $new_len = length($$event_data_ref);
    substr($$ehdr_ref, 0, 3) = pack('CCC',
        $new_len & 0xFF,
        ($new_len >> 8) & 0xFF,
        ($new_len >> 16) & 0xFF);

    warn "PROXY_DEBUG: patched un_len to 0xFFFFFFFC at offset $flag_offset\n";
}

1;
