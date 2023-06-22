# Generate list of positions from binlog file, store it to $datadir/binlog.inc

# Note: --force-read is required for encrypted binlog (binlog_hash_enc.test)
--exec $MYSQL_BINLOG --verbose --force-read $binlog_file > $datadir/binlog.out
--perl
  use File::Basename;
  my $dir= dirname($ENV{binlog_file});
  my $in= "${dir}/binlog.out";
  my @pos;
  open (IN, '<', $in) or die "open(): $!";
  while (<IN>) {
    m/^# at (\d+)$/ && do {
      push @pos, $1;
    }
  }
  close IN;
  open (OUT, '>', "$dir/binlog.inc") or die "open(): $!";
  print OUT "--let pos_n= ". @pos, "\n";
  print OUT "--let pos_l= ". join (', ', @pos), "\n";
  close OUT;
  unlink "$in" or die "unlink(): $!";
EOF
