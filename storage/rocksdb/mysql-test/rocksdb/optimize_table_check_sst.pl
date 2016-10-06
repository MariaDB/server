#!/usr/bin/perl

die unless($ARGV[0]);
open(my $f, "<", $ARGV[0]) or die $!;
my @sst;
while(my $l = readline($f)) {
  chomp($l);
  push @sst, int($l);
}

for(my $i= 0; $i < $#sst; $i++) {
  printf("checking sst file reduction on optimize table from %d to %d..\n", $i, $i+1);

  if($sst[$i] - 1000 < $sst[$i+1]) {
    printf("sst file reduction was not enough. %d->%d (minimum 1000kb)\n", $sst[$i], $sst[$i+1]);
    die;
  }else {
    print "ok.\n";
  }
}
exit(0);

