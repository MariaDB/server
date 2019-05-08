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

    if ($badflags != $flags)
    {
	warn "$file: changing $flags to $badflags\n";
	substr ($_, 54, 4) = pack("N", $badflags);
	# Compute and replace the innodb_checksum_algorithm=crc32 checksum
	my $polynomial = 0x82f63b78; # CRC-32C
	if ($page_size == 1024)
	{
	    # ROW_FORMAT=COMPRESSED
	    substr($_,0,4)=pack("N",
				mycrc32(substr($_, 4, 12), 0, $polynomial) ^
				mycrc32(substr($_, 24, 2), 0, $polynomial) ^
				mycrc32(substr($_, 34, $page_size - 34), 0,
					$polynomial));
	}
	else
	{
	    my $ck=pack("N",
			mycrc32(substr($_, 4, 22), 0, $polynomial) ^
			mycrc32(substr($_, 38, $page_size - 38 - 8), 0,
				$polynomial));
	    substr($_, 0, 4) = $ck;
	    substr ($_, $page_size - 8, 4) = $ck;
	}
	syswrite(FILE, $_, $page_size)==$page_size||die "Unable to write $file\n";
    }
    close(FILE);
}
