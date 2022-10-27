use strict;
use warnings;
use Fcntl qw(:DEFAULT :seek);
do "$ENV{MTR_SUITE_DIR}/../innodb/include/crc32.pl";

sub corrupt_space_page_id {
  my $file_name = shift;
  my @pages_to_corrupt = @_;

  my $page_size = $ENV{INNODB_PAGE_SIZE};

  sysopen my $ibd_file, $file_name, O_RDWR || die "Cannot open $file_name\n";
  sysread($ibd_file, $_, 38) || die "Cannot read $file_name\n";
  my $space = unpack("x[34]N", $_);
  foreach my $page_no (@pages_to_corrupt) {
    $space += 10; # generate wrong space id
    sysseek($ibd_file, $page_size * $page_no, SEEK_SET)
      || die "Cannot seek $file_name\n";

    my $head = pack("Nx[18]", $page_no + 10); # generate wrong page number
    my $body = chr(0) x ($page_size - 38 - 8);

    # Calculate innodb_checksum_algorithm=crc32 for the unencrypted page.
    # The following bytes are excluded:
    # bytes 0..3 (the checksum is stored there)
    # bytes 26..37 (encryption key version, post-encryption checksum, tablespace id)
    # bytes $page_size-8..$page_size-1 (checksum, LSB of FIL_PAGE_LSN)
    my $polynomial = 0x82f63b78; # CRC-32C
    my $ck = mycrc32($head, 0, $polynomial) ^ mycrc32($body, 0, $polynomial);

    my $page= pack("N",$ck).$head.pack("NNN",1,$ck,$space).$body.pack("Nx[4]",$ck);
    die unless syswrite($ibd_file, $page, $page_size) == $page_size;
  }
  close $ibd_file;
}

sub extend_space {
  my $file_name = shift;
  my $n_pages = shift;

  my $page_size = $ENV{INNODB_PAGE_SIZE};
  my $page;

  sysopen my $ibd_file, $file_name, O_RDWR || die "Cannot open $file_name\n";
  sysread($ibd_file, $page, $page_size)
    || die "Cannot read $file_name\n";
  my $size = unpack("N", substr($page, 46, 4));
  my $packed_new_size = pack("N", $size + $n_pages);
  substr($page, 46, 4, $packed_new_size);

  my $head = substr($page, 4, 22);
  my $body = substr($page, 38, $page_size - 38 - 8);
  my $polynomial = 0x82f63b78; # CRC-32C
  my $ck = mycrc32($head, 0, $polynomial) ^ mycrc32($body, 0, $polynomial);
  my $packed_ck = pack("N", $ck);
  substr($page, 0, 4, $packed_ck);
  substr($page, $page_size - 8, 4, $packed_ck);

  sysseek($ibd_file, 0, SEEK_SET)
      || die "Cannot seek $file_name\n";
  die unless syswrite($ibd_file, $page, $page_size) == $page_size;

  sysseek($ibd_file, 0, SEEK_END)
      || die "Cannot seek $file_name\n";
  my $pages_size = $page_size*$n_pages;
  my $pages = chr(0) x $pages_size;
  die unless syswrite($ibd_file, $pages, $pages_size) == $pages_size;
  close $ibd_file;
  return $size;
}

sub die_if_page_is_not_zero {
  my $file_name = shift;
  my @pages_to_check = @_;

  no locale;
  my $page_size = $ENV{INNODB_PAGE_SIZE};
  my $zero_page = chr(0) x $page_size;
  sysopen my $ibd_file, $file_name, O_RDWR || die "Cannot open $file_name\n";
  foreach my $page_no_to_check (@pages_to_check) {
    sysseek($ibd_file, $page_size*$page_no_to_check, SEEK_SET) ||
      die "Cannot seek $file_name\n";
    sysread($ibd_file, my $read_page, $page_size) ||
      die "Cannot read $file_name\n";
    die "The page $page_no_to_check is not zero-filed in $file_name"
      if ($read_page cmp $zero_page);
  }
  close $ibd_file;
}

sub print_corrupted_pages_file {
  my $file_in = shift;
  my $file_out = shift;
  open my $fh, '<', $file_in || die $!;
  my $line_number = 0;
  my $space = {};
  my @spaces;
  while (my $line = <$fh>) {
    ++$line_number;
    if ($line_number & 1) {
      my ($name, $id) = split(/ /, $line);
      $space->{name} = $name;
    }
    else {
      $space->{pages} = $line;
      push (@spaces, $space);
      $space = {};
    }
  }
  close $fh;
  my @sorted_spaces = sort { $a->{name} cmp $b->{name} } @spaces;
  open $fh, '>', $file_out || die $!;
  foreach my $space (@sorted_spaces) {
    print $fh $space->{name};
    print $fh "\n";
    print $fh $space->{pages};
  }
  close $fh;
}

sub append_corrupted_pages {
  my $file_name = shift;
  my $space_name = shift;
  my $pages = shift;
  open my $fh, '<', $file_name || die $!;
  my $line_number = 0;
  my $space_line;
  while (my $line = <$fh>) {
    ++$line_number;
    if ($line_number & 1) {
      my ($name, $id) = split(/ /, $line);
      if ($name eq $space_name) {
        $space_line = $line;
        last;
      }
    }
  }
  close $fh;
  if (not defined $space_line) {
    die "Can't find requested space $space_name in file $file_name";
  }
  open $fh, '>>', $file_name || die $!;
  print $fh $space_line;
  print $fh "$pages\n";
  close $fh;
}
