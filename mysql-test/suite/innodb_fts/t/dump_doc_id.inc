--perl
  use Fcntl qw(:DEFAULT);
  my $ibd_file= "$ENV{MYSQLD_DATADIR}test/$ENV{dump_table}.ibd";

  my $size= 51;
  my $seek= 0xc07f;
  open(FILE, "<", "$ibd_file") or die "$ibd_file: $!\n";
  for (my $i= 0; $i < 2; ++$i) {
    sysseek(FILE, $seek, SEEK_SET) or die "$ibd_file: $!\n";
    my $ret= sysread(FILE, $_, $size);
    if ($ret != $size) {
      if (!defined $ret) {
        die "$ibd_file: failed to read $size bytes: $!\n";
      }
      die "$ibd_file: could read only $ret bytes!\n";
    }
    my @fld= unpack("H12 H12 H14 a3 H14 H14 H16", $_);
    my $f= splice(@fld, 3, 1);
    my ($DB_ROW_ID, $DB_TRX_ID, $DB_ROLL_PTR, $row_start, $row_end, $doc_id)= map { hex($_) } @fld;
    print "DB_ROW_ID=$DB_ROW_ID; DB_TRX_ID=$DB_TRX_ID; DB_ROLL_PTR=$DB_ROLL_PTR; FTS_DOC_ID=$doc_id\n";
    print join(' ', @fld), " $f\n";
    $seek+= $size;
  }

  # /** The bit pattern corresponding to TRX_ID_MAX */
  # const byte trx_id_max_bytes[8] = {
  # 	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
  # };
  #
  # /** The bit pattern corresponding to max timestamp */
  # const byte timestamp_max_bytes[7] = {
  # 	0x7f, 0xff, 0xff, 0xff, 0x0f, 0x42, 0x3f
  # };

  #print $_, "\n";
  close FILE;
EOF
