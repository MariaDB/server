#!/usr/bin/perl
use strict;
use warnings;

my $filename = $ARGV[0] or die "Usage: $0 <logfile>\n";

# Read all lines into memory
open(my $fh, '<', $filename) or die "Cannot open file '$filename': $!";
my @lines = <$fh>;
close($fh);

# Process lines and normalize query IDs
my $first_id;
foreach my $line (@lines) {
  if ($line =~ /,(\d+),QUERY/) {
    $first_id = $1 unless defined $first_id;
    my $normalized = $1 - $first_id;
    $line =~ s/,$1,QUERY/,$normalized,QUERY/;
  }
}

# Write back to the same file
open($fh, '>', $filename) or die "Cannot write to file '$filename': $!";
print $fh @lines;
close($fh);
