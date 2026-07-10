# The following is Public Domain / Creative Commons CC0 from
# http://billauer.co.il/blog/2011/05/perl-crc32-crc-xs-module/

sub mycrc32 {
 my ($input, $init_value, $polynomial) = @_;

 $init_value = 0 unless (defined $init_value);
 $polynomial = 0xedb88320 unless (defined $polynomial);

 my @lookup_table;

 for (my $i=0; $i<256; $i++) {
   my $x = $i;
   for (my $j=0; $j<8; $j++) {
     if ($x & 1) {
       $x = ($x >> 1) ^ $polynomial;
     } else {
       $x = $x >> 1;
     }
   }
   push @lookup_table, $x;
 }

 my $crc = $init_value ^ 0xffffffff;

 foreach my $x (unpack ('C*', $input)) {
   $crc = (($crc >> 8) & 0xffffff) ^ $lookup_table[ ($crc ^ $x) & 0xff ];
 }

 $crc = $crc ^ 0xffffffff;

 return $crc;
}


# Fix the checksum of an InnoDB tablespace page.
# Inputs:
#   $page        A bytestring with the page data.
#   $full_crc32  Checksum type, see get_full_crc32() in innodb-util.pl
# Returns: the modified page as a bytestring.
sub fix_page_crc {
  my ($page, $full_crc32)= @_;
  my $ps= length($page);
  my $polynomial = 0x82f63b78; # CRC-32C
  if ($full_crc32) {
    my $ck = mycrc32(substr($page, 0, $ps - 4), 0, $polynomial);
    substr($page, $ps - 4, 4) = pack("N", $ck);
  } else {
    my $ck= pack("N",
                 mycrc32(substr($page, 4, 22), 0, $polynomial) ^
                 mycrc32(substr($page, 38, $ps - 38 - 8), 0, $polynomial));
    substr($page, 0, 4)= $ck;
    substr($page, $ps-8, 4)= $ck;
  }
  return $page;
}
