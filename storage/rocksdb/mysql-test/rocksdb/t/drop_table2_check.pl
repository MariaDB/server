#!/usr/bin/perl

my $a = 0;
my $b=0;
die unless($ARGV[0]);
open(my $f, "<", $ARGV[0]) or die $!;
while(readline($f)) {
  if (/(\d+) before/) {
    $a = $1;
  }

  if (/(\d+) after/ ) {
    $b = $1;
  }
}

if ($a > $b * 2) {
  printf("Compacted\n");
}
