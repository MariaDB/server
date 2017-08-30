# Convert tablespace flags to the format understood by MariaDB 10.1.0..10.1.20,
# with the assumption that the flags were correct.

sub convert_to_mariadb_101
{
    my ($file, $page_size) = @_;
    open(FILE, "+<", $file) or die "Unable to open $file\n";
    sysread(FILE, $_, $page_size)==$page_size||die "Unable to read $file\n";
    sysseek(FILE, 0, 0)||die "Unable to seek $file\n";

    # FIL_PAGE_DATA + FSP_SPACE_FLAGS = 38 + 16 = 54 bytes from the start
    my($flags) = unpack "x[54]N", $_;
    my $badflags = ($flags & 0x3f);
    my $compression_level=3;
    $badflags |= 1<<6|$compression_level<<7 if ($flags & 1 << 16);
    $badflags |= ($flags & 15 << 6) << 7; # PAGE_SSIZE

    substr ($_, 54, 4) = pack("N", $badflags);
    # Replace the innodb_checksum_algorithm=none checksum
    substr ($_, 0, 4) = pack("N", 0xdeadbeef);
    substr ($_, $page_size - 8, 4) = pack("N", 0xdeadbeef);
    syswrite(FILE, $_, $page_size)==$page_size||die "Unable to write $file\n";
    close(FILE);
}
