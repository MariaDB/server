# -*- cperl -*-

use strict;
use warnings;
use FindBin;
use lib "$FindBin::Bin/..";
use Test::More;
use My::Test;

for my $case ([4, 2, [2, 2]], [5, 3, [2, 2, 1]],
              [2, 4, [1, 1]], [3, 1, [3]], [1, 2, [1]])
{
  my ($repeat, $parallel, $counts)= @$case;
  my $original= My::Test->new(name => 'mtr.parallel_repeat',
                            key => 'original', options => ['--example']);
  my @tests= $original->split_repetitions($repeat, $parallel);
  is_deeply([map { $_->{repeat_count} } @tests], $counts,
            "$repeat repetitions on $parallel workers: batch sizes");
  is_deeply([map { $_->{key} } @tests], [(undef) x @$counts],
            'batches get new identities when dispatched');
  my %addresses= map { ("$_", 1) } @tests;
  is(scalar keys %addresses, scalar @$counts, 'distinct test objects');
  push @{$tests[0]->{options}}, '--changed';
  is_deeply($original->{options}, ['--example'], 'source options unchanged');
  is($original->{key}, 'original', 'source identity unchanged');
  ok(!exists $original->{repeat_count}, 'source repetition count unchanged');
  if (@tests > 1)
  {
    is_deeply($tests[1]->{options}, ['--example'], 'batch options isolated');
    $tests[0]->{failures}= 1;
    ok(!exists $tests[1]->{failures}, 'batch failure history isolated');
  }
}
my $skipped= My::Test->new(skip => 1);
my @skipped= $skipped->split_repetitions(5, 3);
is_deeply(\@skipped, [$skipped], 'statically skipped tests are not duplicated');
done_testing();
